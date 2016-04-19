#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ipc.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include "fifosr.h"
#include "vmm.h"

/* process页表 */

Ptr_ProcessItem process[PROC_MAXN];

/* 实存空间 */
BYTE actMem[ACTUAL_MEMORY_SIZE];
/* 用文件模拟辅存空间 */
FILE *ptr_auxMem;
/* 物理块使用标识 */
BOOL blockStatus[BLOCK_SUM];
/* 访存请求 */
Ptr_MemoryAccessRequest ptr_memAccReq;

int fd, fd2, goon=1;

char statbuff[1<<15];



void synch()
{
	goon=0;
	printf("sure\n");
}

void echo_s()
{
	char s='s';
	write(fd2,&s,1);
}

void echo_f()
{
	char f='f';
	write(fd2,&f,1);
}


void echo_c(BYTE x)
{
	write(fd2,&x,1);
}


/* 初始化环境 */
void do_init(int auxID, Ptr_PageTableItem pageTable)
{
	int i, j;
	srandom(time(NULL));
	for (i = 0; i < PAGE_SUM; i++)
	{
		pageTable[i].pageNum = i;
		pageTable[i].filled = FALSE;
		pageTable[i].edited = FALSE;
		pageTable[i].count = 0;
		/* 使用随机数设置该页的保护类型 */
		switch (random() % 7)
		{
			case 0:
			{
				pageTable[i].proType = READABLE;
				break;
			}
			case 1:
			{
				pageTable[i].proType = WRITABLE;
				break;
			}
			case 2:
			{
				pageTable[i].proType = EXECUTABLE;
				break;
			}
			case 3:
			{
				pageTable[i].proType = READABLE | WRITABLE;
				break;
			}
			case 4:
			{
				pageTable[i].proType = READABLE | EXECUTABLE;
				break;
			}
			case 5:
			{
				pageTable[i].proType = WRITABLE | EXECUTABLE;
				break;
			}
			case 6:
			{
				pageTable[i].proType = READABLE | WRITABLE | EXECUTABLE;
				break;
			}
			default:
				break;
		}

		pageTable[i].proType|=READABLE|WRITABLE;

		/* 设置该页对应的辅存地址 */
		pageTable[i].auxAddr = auxID *VIRTUAL_MEMORY_SIZE + i * PAGE_SIZE;
	}
}


/* 响应请求 */
void do_response()
{
	Ptr_PageTableItem ptr_pageTabIt;
	unsigned int pageNum, offAddr;
	unsigned int actAddr;
	int i;
	Ptr_PageTableItem pageTable=NULL;

	for (i=0;i<PROC_MAXN;++i)
	if (process[i]!=NULL&&process[i]->pid==ptr_memAccReq->pid)
	{
		pageTable=process[i]->paget;
		break;
	}

	if (pageTable==NULL)
	{
		printf("pid %d not found.\n",ptr_memAccReq->pid);
		return;
	}

	/* 检查地址是否越界 */
	if (ptr_memAccReq->virAddr < 0 || ptr_memAccReq->virAddr >= VIRTUAL_MEMORY_SIZE)
	{
		do_error(ERROR_OVER_BOUNDARY);
		return;
	}
	
	/* 计算页号和页内偏移值 */
	pageNum = ptr_memAccReq->virAddr / PAGE_SIZE;
	offAddr = ptr_memAccReq->virAddr % PAGE_SIZE;
	printf("页号为：%u\t页内偏移为：%u\n", pageNum, offAddr);

	/* 获取对应页表项 */
	ptr_pageTabIt = &pageTable[pageNum];
	
	/* 根据特征位决定是否产生缺页中断 */
	if (!ptr_pageTabIt->filled)
	{
		do_page_fault(ptr_pageTabIt, pageTable);
	}
	
	actAddr = ptr_pageTabIt->blockNum * PAGE_SIZE + offAddr;
	printf("实地址为：%u\n", actAddr);
	
	/* 检查页面访问权限并处理访存请求 */
	switch (ptr_memAccReq->reqType)
	{
		case REQUEST_READ: //读请求
		{
			ptr_pageTabIt->count++;
			if (!(ptr_pageTabIt->proType & READABLE)) //页面不可读
			{
				do_error(ERROR_READ_DENY);
				echo_f();			
				return;
			}
			/* 读取实存中的内容 */
			printf("读操作成功：值为%02X\n", actMem[actAddr]);
			echo_s();
			echo_c(actMem[actAddr]);
			break;
		}
		case REQUEST_WRITE: //写请求
		{
			ptr_pageTabIt->count++;
			if (!(ptr_pageTabIt->proType & WRITABLE)) //页面不可写
			{
				do_error(ERROR_WRITE_DENY);	
				echo_f();			
				return;
			}
			/* 向实存中写入请求的内容 */
			actMem[actAddr] = ptr_memAccReq->value;
			ptr_pageTabIt->edited = TRUE;			
			printf("写操作成功\n");
			echo_s();
			break;
		}
		case REQUEST_EXECUTE: //执行请求
		{
			ptr_pageTabIt->count++;
			if (!(ptr_pageTabIt->proType & EXECUTABLE)) //页面不可执行
			{
				do_error(ERROR_EXECUTE_DENY);
				echo_f();			
				return;
			}			
			printf("执行成功\n");
			echo_s();
			break;
		}
		default: //非法请求类型
		{	
			do_error(ERROR_INVALID_REQUEST);
			echo_f();			
			return;
		}
	}
}

/* 处理缺页中断 */
void do_page_fault(Ptr_PageTableItem ptr_pageTabIt, Ptr_PageTableItem pageTable)
{
	unsigned int i;
	printf("产生缺页中断，开始进行调页...\n");
	for (i = 0; i < BLOCK_SUM; i++)
	{
		if (!blockStatus[i])
		{
			/* 读辅存内容，写入到实存 */
			do_page_in(ptr_pageTabIt, i);
			
			/* 更新页表内容 */
			ptr_pageTabIt->blockNum = i;
			ptr_pageTabIt->filled = TRUE;
			ptr_pageTabIt->edited = FALSE;
			ptr_pageTabIt->count = 0;
			
			blockStatus[i] = TRUE;
			return;
		}
	}
	/* 没有空闲物理块，进行页面替换 */
	do_LFU(ptr_pageTabIt, pageTable);
}

/* 根据LFU算法进行页面替换 */
void do_LFU(Ptr_PageTableItem ptr_pageTabIt, Ptr_PageTableItem pageTable)
{
	unsigned int i, min, page;
	printf("没有空闲物理块，开始进行LFU页面替换...\n");
	for (i = 0, min = 0xFFFFFFFF, page = 0; i < PAGE_SUM; i++)
	{
		if (pageTable[i].count < min && pageTable[i].filled)
		{
			min = pageTable[i].count;
			page = i;
		}
	}
	printf("选择第%u页进行替换\n", page);
	if (pageTable[page].edited)
	{
		/* 页面内容有修改，需要写回至辅存 */
		printf("该页内容有修改，写回至辅存\n");
		do_page_out(&pageTable[page]);
	}
	pageTable[page].filled = FALSE;
	pageTable[page].count = 0;


	/* 读辅存内容，写入到实存 */
	do_page_in(ptr_pageTabIt, pageTable[page].blockNum);
	
	/* 更新页表内容 */
	ptr_pageTabIt->blockNum = pageTable[page].blockNum;
	ptr_pageTabIt->filled = TRUE;
	ptr_pageTabIt->edited = FALSE;
	ptr_pageTabIt->count = 0;
	printf("页面替换成功\n");
}

/* 将辅存内容写入实存 */
void do_page_in(Ptr_PageTableItem ptr_pageTabIt, unsigned int blockNum)
{
	unsigned int readNum;
	if (fseek(ptr_auxMem, ptr_pageTabIt->auxAddr, SEEK_SET) < 0)
	{
#ifdef DEBUG
		printf("DEBUG: auxAddr=%u\tftell=%u\n", ptr_pageTabIt->auxAddr, ftell(ptr_auxMem));
#endif
		do_error(ERROR_FILE_SEEK_FAILED);
		exit(1);
	}
	if ((readNum = fread(actMem + blockNum * PAGE_SIZE, 
		sizeof(BYTE), PAGE_SIZE, ptr_auxMem)) < PAGE_SIZE)
	{
#ifdef DEBUG
		printf("DEBUG: auxAddr=%u\tftell=%u\n", ptr_pageTabIt->auxAddr, ftell(ptr_auxMem));
		printf("DEBUG: blockNum=%u\treadNum=%u\n", blockNum, readNum);
		printf("DEGUB: feof=%d\tferror=%d\n", feof(ptr_auxMem), ferror(ptr_auxMem));
#endif
		do_error(ERROR_FILE_READ_FAILED);
		exit(1);
	}
	printf("调页成功：辅存地址%u-->>物理块%u\n", ptr_pageTabIt->auxAddr, blockNum);
}

/* 将被替换页面的内容写回辅存 */
void do_page_out(Ptr_PageTableItem ptr_pageTabIt)
{
	unsigned int writeNum;
	if (fseek(ptr_auxMem, ptr_pageTabIt->auxAddr, SEEK_SET) < 0)
	{
#ifdef DEBUG
		printf("DEBUG: auxAddr=%u\tftell=%u\n", ptr_pageTabIt, ftell(ptr_auxMem));
#endif
		do_error(ERROR_FILE_SEEK_FAILED);
		exit(1);
	}
	if ((writeNum = fwrite(actMem + ptr_pageTabIt->blockNum * PAGE_SIZE, 
		sizeof(BYTE), PAGE_SIZE, ptr_auxMem)) < PAGE_SIZE)
	{
#ifdef DEBUG
		printf("DEBUG: auxAddr=%u\tftell=%u\n", ptr_pageTabIt->auxAddr, ftell(ptr_auxMem));
		printf("DEBUG: writeNum=%u\n", writeNum);
		printf("DEGUB: feof=%d\tferror=%d\n", feof(ptr_auxMem), ferror(ptr_auxMem));
#endif
		do_error(ERROR_FILE_WRITE_FAILED);
		exit(1);
	}
	printf("写回成功：物理块%u-->>辅存地址%03X\n", ptr_pageTabIt->auxAddr, ptr_pageTabIt->blockNum);
}


/* 产生访存请求 */
void do_request(int pid)
{
	/* 随机产生请求地址 */
	ptr_memAccReq->pid=pid;
	ptr_memAccReq->virAddr = random() % VIRTUAL_MEMORY_SIZE;
	/* 随机产生请求类型 */
	switch (random() %5 % 3)
	{
		case 0: //读请求
		{
			ptr_memAccReq->reqType = REQUEST_READ;
			printf("产生请求：\n地址：%u\t类型：读取\n", ptr_memAccReq->virAddr);
			break;
		}
		case 1: //写请求
		{
			ptr_memAccReq->reqType = REQUEST_WRITE;
			/* 随机产生待写入的值 */
			ptr_memAccReq->value = random() % 0xFFu;
			printf("产生请求：\n地址：%u\t类型：写入\t值：%02X\n", ptr_memAccReq->virAddr, ptr_memAccReq->value);
			break;
		}
		case 2:
		{
			ptr_memAccReq->reqType = REQUEST_EXECUTE;
			printf("产生请求：\n地址：%u\t类型：执行\n", ptr_memAccReq->virAddr);
			break;
		}
		default:
			break;
	}	
}

/* 打印页表 */
void do_print_info(int x, int writeback)
{
	unsigned int i, j, k, offset=0;
	char str[4];
	Ptr_PageTableItem pageTable;
	if (x<4)
		pageTable=process[x]->paget;
	else 
	{
		for (i=0;i<PROC_MAXN;++i) 
			if (process[i]!=NULL&&process[i]->pid==x)
			{
				x=i;
				break;
			}
		pageTable=process[x]->paget;		
	}
	if (x<0||x>3) {
		printf("pid %d not found.\n",x);
		if (writeback) echo_f();
		return;
	}
	offset+=sprintf(statbuff+offset,"页号\t块号\t装入\t修改\t保护\t计数\t辅存\n");
	for (i = 0; i < PAGE_SUM; i++)
	{
		offset+=sprintf(statbuff+offset,"%u\t%u\t%u\t%u\t%s\t%u\t%u\n", i, pageTable[i].blockNum, pageTable[i].filled, 
			pageTable[i].edited, get_proType_str(str, pageTable[i].proType), 
			pageTable[i].count, pageTable[i].auxAddr);
	}
	statbuff[offset]=0;
	if (writeback)
	{
		echo_s();
		write(fd2,statbuff,offset);
		echo_c('$');
	}
	printf("%s\n", statbuff);
}

/* 获取页面保护类型字符串 */
char *get_proType_str(char *str, BYTE type)
{
	if (type & READABLE)
		str[0] = 'r';
	else
		str[0] = '-';
	if (type & WRITABLE)
		str[1] = 'w';
	else
		str[1] = '-';
	if (type & EXECUTABLE)
		str[2] = 'x';
	else
		str[2] = '-';
	str[3] = '\0';
	return str;
}

void dispose()
{
	int i;
	struct stat statb;	
	close(fd);
	close(fd2);
	if (fclose(ptr_auxMem) == EOF)
	{
		do_error(ERROR_FILE_CLOSE_FAILED);
		exit(1);
	}
	for (i=0;i<PROC_MAXN;++i) 
		if (process[i]!=NULL) free(process[i]);
	if(stat(REQFIFO,&statb)==0){
		remove(REQFIFO);
	}
	if(stat(RESFIFO,&statb)==0){
		remove(RESFIFO);
	}
	printf("bye\n");
	exit(0);
}

void new_proc(int pid)
{
	int i;
	for (i=0;i<PROC_MAXN;++i)
	if (process[i]==NULL)
	{
		process[i]=(Ptr_ProcessItem)malloc(sizeof(ProcessItem));
		process[i]->pid=pid;
		do_init(i,process[i]->paget);
		//do_print_info(i,0);
		echo_s();
		i=getpid();
		write(fd2,&i,4);		
		return;
	}
	printf("Insufficient Memory.\n");
	echo_f();
}

void free_proc(int pid)
{
	int i,j;
	for (i=0;i<PROC_MAXN;++i)
	if (process[i]!=NULL&&process[i]->pid==pid)
	{
		for (j=0;j<PAGE_SUM;++j)
		{
			if (process[i]->paget[j].filled)
			{
				blockStatus[process[i]->paget[j].blockNum]=FALSE;
			}
		}
		free(process[i]);
		process[i]=NULL;
		printf("rm: pid %d\n", pid);
		echo_s();
		return;
	}
	printf("pid %d not found.\n",pid);
	echo_f();
}


void environmentInit()
{
	int i,j;
	struct stat statb;	
	char c=0;
	printf("Initiailizing...\n");
	if (!(ptr_auxMem = fopen(AUXILIARY_MEMORY, "r+")))
	{
		if (!(ptr_auxMem = fopen(AUXILIARY_MEMORY, "w+")))
		{
			do_error(ERROR_FILE_OPEN_FAILED);
			exit(1);
		}
		else {
			for (i=0;i<DISK_SIZE;++i) fwrite(&c,1,1,ptr_auxMem);
			fclose(ptr_auxMem);
			if (!(ptr_auxMem = fopen(AUXILIARY_MEMORY, "r+")))
			{
				do_error(ERROR_FILE_OPEN_FAILED);
				exit(1);
			}
		}
	}
	printf("DISK STATUS: ready\n");
	signal(SIGINT,dispose);
	signal(SIGUSR1,synch);
	for (i=0;i<PROC_MAXN;++i) process[i]=NULL;
	ptr_memAccReq = (Ptr_MemoryAccessRequest) malloc(sizeof(MemoryAccessRequest));
	for (j = 0; j < BLOCK_SUM; j++)
	{
		blockStatus[j] = FALSE;
	}

	if(stat(REQFIFO,&statb)==0){
		/*  */
		if(remove(REQFIFO)<0)
		{
			do_error(ERROR_FILE_OPEN_FAILED);
			dispose();
		}
	}

	if(mkfifo(REQFIFO,0777)<0||(fd=open(REQFIFO,O_RDONLY))<0)
	{
		do_error(ERROR_FILE_OPEN_FAILED);
		dispose();
	}

	printf("REQFIFO STATUS: ready\n");

	printf("started.\n");
}

char getReq()
{
	char c=' ';
	int count, pid=0;
	while ((count = read(fd,&c,1))<=0)
	{
		if (count<0)
		{
			do_error(ERROR_FILE_OPEN_FAILED);
			dispose();
		}
	}
	if (count==0||c!='i'&&c!='e'&&c!='s'&&c!='r')
		return ' ';

	printf("getReq: %c\n",c);

	if (c=='i') 
	{
		count=0;
		while ((fd2=open(RESFIFO,O_WRONLY))<0) 
		{
			++count;
			sleep(0.1);
			if (count>100)
			{
				do_error(ERROR_FILE_OPEN_FAILED);
				dispose();
			}
		}		
	}
	else 
	{
		while (goon);
		if ((fd2=open(RESFIFO,O_WRONLY))<0) 
		{
			do_error(ERROR_FILE_OPEN_FAILED);
			dispose();
		}
		goon=1;
	}

	if((count = read(fd,&pid,4))<=0)
	{
		do_error(ERROR_FILE_OPEN_FAILED);
		dispose();
	}

	printf("getPid: %d\n",pid);
	ptr_memAccReq->pid=pid;
	if (c=='r')
	{
		if((count = read(fd,ptr_memAccReq,sizeof(MemoryAccessRequest)))<=0)
		{
			do_error(ERROR_FILE_OPEN_FAILED);
			dispose();
		}

	}
	return c;
}

int main(int argc, char* argv[])
{
	char c=0;
	environmentInit();
	/* 在循环中模拟访存请求与处理过程 */
	while (TRUE)
	{
		switch (getReq())
		{
			case 'e':
				free_proc(ptr_memAccReq->pid);
				break;			
			case 'i':
				new_proc(ptr_memAccReq->pid);
				break;			
			case 's':
				do_print_info(ptr_memAccReq->pid,1);
				break;
			case 'r':
				do_response();
				break;
			default:
				break;				
		}
		close(fd2);
		//sleep(5000);
	}

	return 0;
}
