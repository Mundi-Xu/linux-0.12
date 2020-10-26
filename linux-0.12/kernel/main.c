/*
 *  linux/init/main.c
 *
 *  (C) 1991  Linus Torvalds
 */

#define __LIBRARY__
#include <unistd.h>
#include <time.h> // 时间类型头文件。其中最主要定义了 tm 结构和一些有关时间的函数原形

/*
 * we need this inline - forking from kernel space will result
 * in NO COPY ON WRITE (!!!), until an execve is executed. This
 * is no problem, but for the stack. This is handled by not letting
 * main() use the stack at all after fork(). Thus, no function
 * calls - which means inline code for fork too, as otherwise we
 * would use the stack upon exit from 'fork()'.
 *
 * Actually only pause and fork are needed inline, so that there
 * won't be any messing with the stack from main(), but we define
 * some others too.

	Linux 在内核空间创建进程时不使用写时复制技术（Copy on write）。main()在移动到用户
	模式（到任务 0）后执行内嵌方式的 fork()和 pause()，因此可保证不使用任务 0 的用户栈。
	在执行 moveto_user_mode()之后，本程序 main()就以任务 0 的身份在运行了。而任务 0 是所
	有将创建子进程的父进程。当它创建一个子进程时（init 进程），由于任务 1 代码属于内核
	空间，因此没有使用写时复制功能。此时任务 0 的用户栈就是任务 1 的用户栈，即它们共同
	使用一个栈空间。因此希望在 main.c 运行在任务 0 的环境下时不要有对堆栈的任何操作，以
	免弄乱堆栈。而在再次执行 fork()并执行过 execve()函数后，被加载程序已不属于内核空间，
	因此可以使用写时复制技术了。
*/
static inline _syscall0(int,fork) //int fork()创建进程系统调用
static inline _syscall0(int,pause) //int pause()系统调用：暂停进程的执行，直到收到一个信号。
static inline _syscall1(int,setup,void *,BIOS) 
// int setup(void * BIOS)系统调用，仅用于 linux 初始化（仅在这个程序中被调用）。
static inline _syscall0(int,sync) //int sync()系统调用：更新文件系统。

#include <linux/tty.h>
#include <linux/sched.h>
#include <linux/head.h>
#include <asm/system.h>
#include <asm/io.h>

#include <stddef.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>

#include <linux/fs.h>

#include <string.h>

static char printbuf[1024];

extern char *strcpy(); 
extern int vsprintf(); // 传格式化输出到一字符串中
extern void init(void);
extern void blk_dev_init(void); // 块设备初始化子程序
extern void chr_dev_init(void); // 字符设备初始化
extern void hd_init(void); // 硬盘初始化程序
extern void floppy_init(void); // 软驱初始化程序
extern void mem_init(long start, long end); // 内存管理初始化
extern long rd_init(long mem_start, int length); // 虚拟盘初始化
extern long kernel_mktime(struct tm * tm); // 计算系统开机启动时间（秒）

/*
    @description: 内核专用 sprintf()函数

	该函数用于产生格式化信息并输出到指定缓冲区 str 中。
	参数'*fmt'指定输出将采用的格式。
	函数使用 vsprintf()将格式化字符串放入 str 缓冲区。
*/

static int sprintf(char * str, const char *fmt, ...)
{
	va_list args;
	int i;

	va_start(args, fmt);
	i = vsprintf(str, fmt, args);
	va_end(args);
	return i;
}

/*
 *  This is set up by the setup-routine at boot-time

	下面三行分别将指定的线性地址强行转换为给定数据类型的指针，并获取指针所指内容。由于
	内核代码段被映射到从物理地址零开始的地方，因此这些线性地址正好也是对应的物理地址。
*/

#define EXT_MEM_K (*(unsigned short *)0x90002)
#define CON_ROWS ((*(unsigned short *)0x9000e) & 0xff)
#define CON_COLS (((*(unsigned short *)0x9000e) & 0xff00) >> 8)
#define DRIVE_INFO (*(struct drive_info *)0x90080)
#define ORIG_ROOT_DEV (*(unsigned short *)0x901FC)
#define ORIG_SWAP_DEV (*(unsigned short *)0x901FA)

/*
 * Yeah, yeah, it's ugly, but I cannot find how to do this correctly
 * and this seems to work. I anybody has more info on the real-time
 * clock I'd be interested. Most of this was trial and error, and some
 * bios-listing reading. Urghh.
 */

// 读取 CMOS 实时时钟信息。
#define CMOS_READ(addr) ({ \
outb_p(0x80|addr,0x70); \
inb_p(0x71); \
})
// 将 BCD 码转换成二进制数值。
#define BCD_TO_BIN(val) ((val)=((val)&15) + ((val)>>4)*10)

/*
    @description: 读取 CMOS 实时钟信息作为开机时间，并保存到全局变量 startup_time(秒)中。

	其中调用的函数 kernel_mktime()用于计算从 1970 年 1 月 1 日 0 时起
	到开机当日经过的秒数，作为开机时间。
*/

static void time_init(void)
{
	struct tm time;
/*
	CMOS 的访问速度很慢。为了减小时间误差，在读取了下面循环中所有数值后，若此时 CMOS 中
	秒值发生了变化，那么就重新读取所有值。这样内核就能把与 CMOS 时间误差控制在 1 秒之内。
*/
	do {
		time.tm_sec = CMOS_READ(0);
		time.tm_min = CMOS_READ(2);
		time.tm_hour = CMOS_READ(4);
		time.tm_mday = CMOS_READ(7);
		time.tm_mon = CMOS_READ(8);
		time.tm_year = CMOS_READ(9);
	} while (time.tm_sec != CMOS_READ(0));
	BCD_TO_BIN(time.tm_sec);
	BCD_TO_BIN(time.tm_min);
	BCD_TO_BIN(time.tm_hour);
	BCD_TO_BIN(time.tm_mday);
	BCD_TO_BIN(time.tm_mon);
	BCD_TO_BIN(time.tm_year);
	time.tm_mon--;
	startup_time = kernel_mktime(&time);
}

static long memory_end = 0; // 机器具有的物理内存容量（字节数）。
static long buffer_memory_end = 0; // 高速缓冲区末端地址。
static long main_memory_start = 0; // 主内存（将用于分页）开始的位置。
static char term[32]; // 终端设置字符串（环境参数）。
// 读取并执行/etc/rc 文件时所使用的命令行参数和环境参数。
static char * argv_rc[] = { "/bin/sh", NULL };
static char * envp_rc[] = { "HOME=/", NULL ,NULL };
// 运行登录 shell 时所使用的命令行参数和环境参数。
static char * argv[] = { "-/bin/sh",NULL };
static char * envp[] = { "HOME=/usr/root", NULL, NULL };

struct drive_info { char dummy[32]; } drive_info; // 用于存放硬盘参数表信息。

/*
    @description:  内核初始化主程序

	初始化结束后将以任务 0（idle 任务即空闲任务）的身份运行。
*/

void main(void)		/* This really IS void, no error here. */
{			/* The startup routine assumes (well, ...) this */
/*
 * Interrupts are still disabled. Do necessary setups, then
 * enable them
 */

/*
	首先保存根文件系统设备号和交换文件设备号，并根据 setup.s 程序中获取的信息设置控制台
	终端屏幕行、列数环境变量 TERM，并用其设置初始 init 进程中执行 etc/rc 文件和 shell 程序
	使用的环境变量，以及复制内存 0x90080 处的硬盘参数表。
*/
 	ROOT_DEV = ORIG_ROOT_DEV;
 	SWAP_DEV = ORIG_SWAP_DEV;
	sprintf(term, "TERM=con%dx%d", CON_COLS, CON_ROWS);
	envp[1] = term;	
	envp_rc[1] = term;
 	drive_info = DRIVE_INFO;
/*
	接着根据机器物理内存容量设置高速缓冲区和主内存区的位置和范围。
	高速缓存末端地址 -> buffer_memory_end；
	机器内存容量 -> memory_end；
	主内存开始地址 -> main_memory_start；
*/
	memory_end = (1<<20) + (EXT_MEM_K<<10);
	memory_end &= 0xfffff000;
	if (memory_end > 16*1024*1024)
		memory_end = 16*1024*1024;
	if (memory_end > 12*1024*1024) 
		buffer_memory_end = 4*1024*1024;
	else if (memory_end > 6*1024*1024)
		buffer_memory_end = 2*1024*1024;
	else
		buffer_memory_end = 1*1024*1024;
	main_memory_start = buffer_memory_end;
// 如果在 Makefile 文件中定义了内存虚拟盘符号 RAMDISK，则初始化虚拟盘。此时主内存将减少。
#ifdef RAMDISK
	main_memory_start += rd_init(main_memory_start, RAMDISK*1024);
#endif
// 以下是内核进行所有方面的初始化工作。
	mem_init(main_memory_start,memory_end); // 主内存区初始化。
	trap_init(); // 陷阱（硬件中断向量）初始化。
	blk_dev_init(); // 块设备初始化。
	chr_dev_init(); // 字符设备初始化。
	tty_init(); // tty 初始化。
	time_init(); // 设置开机启动时间。
	sched_init(); // 调度程序初始化（加载任务 0 的 tr,ldtr）
	buffer_init(buffer_memory_end); // 缓冲管理初始化，建内存链表等。
	hd_init(); // 硬盘初始化。
	floppy_init(); // 软驱初始化。
	sti(); // 初始化工作完毕，开启中断。
	move_to_user_mode(); // 移到用户模式下执行。
	if (!fork()) {		/* we count on this going ok */
		init(); // 在新建的子进程（任务 1 即 init 进程）中执行。
	}
/*
 *   NOTE!!   For any other task 'pause()' would mean we have to get a
 * signal to awaken, but task0 is the sole exception (see 'schedule()')
 * as task 0 gets activated at every idle moment (when no other tasks
 * can run). For task0 'pause()' just means we go check if some other
 * task can run, and if not we return here.
	
	pause()系统调用（kernel/sched.c,144）会把任务 0 转换成可中断等待状态，再执行调度函数。
	但是调度函数只要发现系统中没有其他任务可以运行时就会切换到任务 0，而不依赖于任务 0 的状态。
*/
	for(;;)
		__asm__("int $0x80"::"a" (__NR_pause):"ax"); // 执行系统调用 pause()。
}

/*
    @description:  产生格式化信息并输出到标准输出设备 stdout(1)

	参数'*fmt'指定输出将采用的格式。该程序使用 vsprintf()将格式化的字符串放入 printbuf 缓冲区，
	然后用 write()将缓冲区的内容输出到标准设备（1--stdout）。
*/

static int printf(const char *fmt, ...)
{
	va_list args;
	int i;

	va_start(args, fmt);
	write(1,printbuf,i=vsprintf(printbuf, fmt, args));
	va_end(args);
	return i;
}

/*
    @description:  	对第一个将要执行的程序（shell）的环境进行初始化，
					然后以登录 shell 方式加载该程序并执行之。

	在 main()中已经进行了系统初始化，包括内存管理、各种硬件设备和驱动程序。
	init()函数运行在任务 0 第 1 次创建的子进程（任务 1）中。
*/

void init(void)
{
	int pid,i;
/*
	setup() 是一个系统调用。用于读取硬盘参数包括分区表信息并加载虚拟盘
	（若存在的话）和安装根文件系统设备。
*/
	setup((void *) &drive_info);
/*
	以读写访问方式打开设备“/dev/tty0”，对应终端控制台。由于这是第一次打开文件
	操作，因此产生的文件句柄号（文件描述符）肯定是 0。该句柄是 UNIX 类操作系统
	默认的控制台标准输入句柄 stdin。这里再把它以读和写的方式分别打开是为了复制
	产生标准输出（写）句柄 stdout 和标准出错输出句柄 stderr。函数前面的
	“(void)”前缀用于表示强制函数无需返回值。
*/
	(void) open("/dev/tty1",O_RDWR,0);
	(void) dup(0);
	(void) dup(0);
// 下面打印缓冲区块数和总字节数，每块 1024 字节，以及主内存区空闲内存字节数。
	printf("%d buffers = %d bytes buffer space\n\r",NR_BUFFERS,
		NR_BUFFERS*BLOCK_SIZE);
	printf("Free mem: %d bytes\n\r",memory_end-main_memory_start);
/*
	fork()用于创建一个子进程（任务 2）。对于被创建的子进程，fork()将返回 0 值，对于
	原进程（父进程）则返回子进程的进程号 pid。该子进程关闭了句柄 0（stdin）、以只读方式
	打开/etc/rc 文件，并使用 execve()函数将进程自身替换成 /bin/sh 程序（即 shell 程序），
	然后执行 /bin/sh 程序。所携带的参数和环境变量分别由 argv_rc 和 envp_rc 数组给出。
	关闭句柄 0 并立刻打开 /etc/rc 文件的作用是把标准输入stdin 重定向到 /etc/rc 文件。
	这样 shell 程序/bin/sh 就可以运行 rc 文件中设置的命令。由于这里 sh 的运行方式是
	非交互式的，因此在执行完 rc 文件中的命令后就会立刻退出，进程 2也随之结束。
	函数_exit()退出时的出错码 1 – 操作未许可；2 -- 文件或目录不存在。
*/
	if (!(pid=fork())) {
		close(0);
		if (open("/etc/rc",O_RDONLY,0))
			_exit(1);
		execve("/bin/sh",argv_rc,envp_rc);
		_exit(2);
	}
/*
	下面还是父进程（1）执行的语句。wait()等待子进程停止或终止，返回值应是子进程的进程号
	(pid)。这三句的作用是父进程等待子进程的结束。&i 是存放返回状态信息的位置。如果 
	wait()返回值不等于子进程号，则继续等待。
*/
	if (pid>0)
		while (pid != wait(&i))
			/* nothing */;
/*
	如果执行到这里，说明刚创建的子进程的执行已停止或终止了。下面循环中首先再创建一个子
	进程，如果出错，则显示“初始化程序创建子进程失败”信息并继续执行。对于所创建的子进程
	将关闭所有以前还遗留的句柄(stdin, stdout, stderr)，新创建一个会话并设置进程组号，
	然后重新打开 /dev/tty0 作为 stdin，并复制成 stdout 和 stderr，再次执行系统解释程序
	/bin/sh。但这次执行所选用的参数和环境数组另选了一套，然后父进程再次运行 wait()等待。
	如果子进程又停止了执行，则在标准输出上显示出错信息“子进程pid 停止了运行，返回码是 i”，
	然后继续重试下去…，形成大死循环。
*/
	while (1) {
		if ((pid=fork())<0) {
			printf("Fork failed in init\r\n");
			continue;
		}
		if (!pid) { // 新的子进程。
			close(0);close(1);close(2);
			setsid(); // 创建一新的会话期
			(void) open("/dev/tty1",O_RDWR,0);
			(void) dup(0);
			(void) dup(0);
			_exit(execve("/bin/sh",argv,envp));
		}
		while (1)
			if (pid == wait(&i))
				break;
		printf("\n\rchild %d died with code %04x\n\r",pid,i);
		sync(); // 同步操作，刷新缓冲区。
	}
	_exit(0);	/* NOTE! _exit, not exit() */
}
