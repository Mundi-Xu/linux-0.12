/*
 *  linux/kernel/sched.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * 'sched.c' is the main kernel file. It contains scheduling primitives
 * (sleep_on, wakeup, schedule etc) as well as a number of simple system
 * call functions (type getpid(), which just extracts a field from
 * current-task
 */
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/sys.h>
#include <linux/fdreg.h>
#include <asm/system.h>
#include <asm/io.h>
#include <asm/segment.h>

#include <signal.h>

#define _S(nr) (1<<((nr)-1))
#define _BLOCKABLE (~(_S(SIGKILL) | _S(SIGSTOP)))

// 内核调试函数。显示任务号 nr 的进程号、进程状态和内核堆栈空闲字节数（大约）。
void show_task(int nr,struct task_struct * p)
{
	int i,j = 4096-sizeof(struct task_struct);

	printk("%d: pid=%d, state=%d, father=%d, child=%d, ",nr,p->pid,
		p->state, p->p_pptr->pid, p->p_cptr ? p->p_cptr->pid : -1);
	i=0;
	while (i<j && !((char *)(p+1))[i]) // 检测指定任务数据结构以后等于 0 的字节数。
		i++;
	printk("%d/%d chars free in kstack\n\r",i,j);
	printk("   PC=%08X.", *(1019 + (unsigned long *) p));
	if (p->p_ysptr || p->p_osptr) 
		printk("   Younger sib=%d, older sib=%d\n\r", 
			p->p_ysptr ? p->p_ysptr->pid : -1,
			p->p_osptr ? p->p_osptr->pid : -1);
	else
		printk("\n\r");
}

// 显示所有任务的任务号、进程号、进程状态和内核堆栈空闲字节数（大约）。
// NR_TASKS 是系统能容纳的最大进程(任务)数量(64 个)

void show_state(void)
{
	int i;

	printk("\rTask-info:\n\r");
	for (i=0;i<NR_TASKS;i++)
		if (task[i])
			show_task(i,task[i]);
}
// PC 机 8253 定时芯片的输入时钟频率约为 1.193180MHz。Linux 内核希望定时器发出中断的频率是
 // 100Hz，也即每 10ms 发出一次时钟中断。
#define LATCH (1193180/HZ)

extern void mem_use(void);

extern int timer_interrupt(void);// 时钟中断处理程序
extern int system_call(void);// 系统调用中断处理程序

// 每个任务（进程）在内核态运行时都有自己的内核态堆栈。这里定义了任务的内核态堆栈结构。
// 这里定义任务联合（任务结构成员和 stack 字符数组成员）。因为一个任务的数据结构与其内核
// 态堆栈放在同一内存页中，所以从堆栈段寄存器 ss 可以获得其数据段选择符。

union task_union {
	struct task_struct task;
	char stack[PAGE_SIZE];
};
// 设置初始任务的数据。初始数据在 include/kernel/sched.h 中
static union task_union init_task = {INIT_TASK,};

// 从开机开始算起的滴答数时间值全局变量（10ms/滴答）。系统时钟中断每发生一次即一个滴答。
// 前面的限定符 volatile 的含义是向编译器指明变量的内容可能会由于被其他程序修改而变化。
// 通常在程序中申明一个变量时， 编译器会尽量把它存放在通用寄存器中，以提高访问效率。
// 当 CPU 把其值放到 ebx 中后一般就不会再关心该变量对应内存位置中的内容。若此时其他程序
//（例如内核程序或一个中断过程）修改了内存中该变量的值，ebx 中的值并不会随之更新。为了
//解决这种情况就创建了 volatile限定符，让代码在引用该变量时一定要从指定内存位置中取得其值。
//这里即是要求 gcc 不要对 jiffies 这个变量进行优化处理，也不要挪动位置，并且需要从内存中取其值。
//因为时钟中断处理过程等程序会修改它的值。
unsigned long volatile jiffies=0;
unsigned long startup_time=0;// 开机时间。从 1970:0:0:0 开始计时的秒数。
int jiffies_offset = 0;		/* # clock ticks to add to get "true
				   time".  Should always be less than
				   1 second's worth.  For time fanatics
				   who like to syncronize their machines
				   to WWV :-) */

struct task_struct *current = &(init_task.task);// 当前任务指针（初始化指向任务 0）。
struct task_struct *last_task_used_math = NULL;// 使用过协处理器任务的指针。

// 定义任务指针数组。第 1 项被初始化指向初始任务（任务 0）的任务数据结构。
struct task_struct * task[NR_TASKS] = {&(init_task.task), };

// 定义用户堆栈，共 1K 项，容量 4K 字节。在内核初始化操作过程中被用作内核栈，初始化完成
 // 以后将被用作任务 0 的用户态堆栈。在运行任务 0 之前它是内核栈，以后用作任务 0 和 1 的用
 // 户态栈。下面结构用于设置堆栈 ss:esp(数据段选择符，指针).
 // ss 被设置为内核数据段选择符（0x10），指针 esp 指在 user_stack 数组最后一项后面。这是
 // 因为 Intel CPU 执行堆栈操作时是先递减堆栈指针 sp 值，然后在 sp 指针处保存入栈内容。
long user_stack [ PAGE_SIZE>>2 ] ;

struct {
	long * a;
	short b;
	} stack_start = { & user_stack [PAGE_SIZE>>2] , 0x10 };
/*
 *  'math_state_restore()' saves the current math information in the
 * old math state array, and gets the new ones from the current task
 */

// 当任务被调度交换过以后，该函数用以保存原任务的协处理器状态（上下文）并恢复新调度进
 // 来的当前任务的协处理器执行状态。
void math_state_restore()
{
	// 如果任务没变则返回
	if (last_task_used_math == current)
		return;
	// 在发送协处理器命令之前要先发 WAIT 指令。如果上个任务使用了协处理器，则保存其状态。
	__asm__("fwait");
	if (last_task_used_math) {
		__asm__("fnsave %0"::"m" (last_task_used_math->tss.i387));
	}
	// 现在，last_task_used_math 指向当前任务，以备当前任务被交换出去时使用。此时如果当前
    // 任务用过协处理器，则恢复其状态。否则的话说明是第一次使用，于是就向协处理器发初始化
    // 命令，并设置使用了协处理器标志。
	last_task_used_math=current;
	if (current->used_math) {
		__asm__("frstor %0"::"m" (current->tss.i387));
	} else {
		__asm__("fninit"::);
		current->used_math=1;
	}
}

/*
 *  'schedule()' is the scheduler function. This is GOOD CODE! There
 * probably won't be any reason to change this, as it should work well
 * in all circumstances (ie gives IO-bound processes good response etc).
 * The one thing you might take a look at is the signal-handler code here.
 *
 *   NOTE!!  Task 0 is the 'idle' task, which gets called when no other
 * tasks can run. It can not be killed, and it cannot sleep. The 'state'
 * information in task[0] is never used.
 */
void schedule(void)
{
	int i,next,c;
	struct task_struct ** p;// 任务结构指针的指针。

/* check alarm, wake up any interruptible tasks that have got a signal */
// 从任务数组中最后一个任务开始循环检测 alarm。在循环时跳过空指针项
	for(p = &LAST_TASK ; p > &FIRST_TASK ; --p)
		if (*p) {
			 // 如果设置过任务超时定时 timeout，并且已经超时，则复位超时定时值，并且如果任务处于可
 			// 中断睡眠状态 TASK_INTERRUPTIBLE 下，将其置为就绪状态（TASK_RUNNING）。
			if ((*p)->timeout && (*p)->timeout < jiffies) {
				(*p)->timeout = 0;
				if ((*p)->state == TASK_INTERRUPTIBLE)
					(*p)->state = TASK_RUNNING;
			}
			// 如果设置过任务的定时值 alarm，并且已经过期(alarm<jiffies),则在信号位图中置 SIGALRM
 			// 信号，即向任务发送 SIGALARM 信号。然后清 alarm。该信号的默认操作是终止进程。
			if ((*p)->alarm && (*p)->alarm < jiffies) {
				(*p)->signal |= (1<<(SIGALRM-1));
				(*p)->alarm = 0;
			}
			// 如果信号位图中除被阻塞的信号外还有其他信号，并且任务处于可中断状态，则置任务为就绪
 			// 状态。其中'~(_BLOCKABLE & (*p)->blocked)'用于忽略被阻塞的信号，但 SIGKILL 和 SIGSTOP
 			// 不能被阻塞。
			if (((*p)->signal & ~(_BLOCKABLE & (*p)->blocked)) &&
			(*p)->state==TASK_INTERRUPTIBLE)
				(*p)->state=TASK_RUNNING;//置为就绪（可执行）状态。
		}

/* this is the scheduler proper: */

	while (1) {
		c = -1;
		next = 0;
		i = NR_TASKS;
		p = &task[NR_TASKS];
		// 这段代码也是从任务数组的最后一个任务开始循环处理，并跳过不含任务的数组槽。比较每个
 		// 就绪状态任务的 counter值，哪一个值大，运行时间还不长，next 就指向哪个的任务号。
		while (--i) {
			if (!*--p)
				continue;
			if ((*p)->state == TASK_RUNNING && (*p)->counter > c)
				c = (*p)->counter, next = i;
		}
		// 如果比较得出有 counter 值不等于 0 的结果，或者系统中没有一个可运行的任务存在（此时 c
		// 仍然为-1，next=0），则退出 循环，执行任务切换操作。否则就根据每个任务的优先权值，更新
		// 每一个任务的 counter 值，然后重新比较。注意，这里计算过程不考虑进程的状态。

		if (c) break;
		for(p = &LAST_TASK ; p > &FIRST_TASK ; --p)
			if (*p)
				(*p)->counter = ((*p)->counter >> 1) +
						(*p)->priority;
	}
	// 用下面宏（定义在 sched.h 中）把当前任务指针 current 指向任务号为 next 的任务，并切换
 	// 到该任务中运行。 next 被初始化为 0，因此若系统中没有任何其他任务可运行时，则 next 始终为 0。
	//因此调度函数会在系统空闲时去执行任务 0。 此时任务 0 仅执行 pause()系统调用，并又会调用本函数。

	switch_to(next);
}
// pause()系统调用。转换当前任务的状态为可中断的等待状态，并重新调度。
 // 该系统调用将导致进程进入睡眠状态，直到收到一个信号。该信号用于终止进程或者使进程
 // 调用一个信号捕获函数。只有当捕获了一个信号，并且信号捕获处理函数返回，pause()才
 // 会返回。此时 pause()返回值应该是 -1，并且 errno 被置为 EINTR。这里还没有完全实现
int sys_pause(void)
{
	current->state = TASK_INTERRUPTIBLE;
	schedule();
	return 0;
}
// 把当前任务置为指定的睡眠状态（可中断的或不可中断的），并让睡眠队列头指针指向当前任务。
 // 函数参数 p 是等待任务队列头指针。指针是含有一个变量地址的变量。这里参数 p 使用了指针的
 // 指针形式 '**p'，这是因为 C 函数参数只能传值，没有直接的方式让被调用函数改变调用该函数
 // 程序中变量的值。但是指针'*p'指向的目标（这里是任务结构）会改变，因此为了能修改调用该
 // 函数程序中原来就是指针变量的值，就需要传递指针'*p'的指针，即'**p'。
 // 参数 state 是任务睡眠使用的状态：TASK_UNINTERRUPTIBLE 或 TASK_INTERRUPTIBLE。处于不可
 // 中断睡眠状态（TASK_UNINTERRUPTIBLE）的任务需要内核程序利用 wake_up()函数明确唤醒之。
 // 处于可中断睡眠状态（TASK_INTERRUPTIBLE）可以通过信号、任务超时等手段唤醒 （置为就绪
 // 状态 TASK_RUNNING）。
static inline void __sleep_on(struct task_struct **p, int state)
{
	struct task_struct *tmp;
	// 若指针无效，则退出。（指针所指的对象可以是 NULL，但指针本身不会为 0)。
 	// 如果当前任务是任务 0，则死机(impossible!)。
	if (!p)
		return;
	if (current == &(init_task.task))
		panic("task[0] trying to sleep");
	// 让 tmp 指向已经在等待队列上的任务(如果有的话)，例如 inode->i_wait。并且将睡眠队列头
 	// 的等待指针指向当前任务。这样就把当前任务插入到了 *p 的等待队列中。然后将当前任务置
 	// 为指定的等待状态，并执行重新调度。
	tmp = *p;
	*p = current;
	current->state = state;
repeat:	schedule();
// 只有当这个等待任务被唤醒时，程序才又会返回到这里，表示进程已被明确地唤醒并执行。
 // 如果等待队列中还有等待任务，并且队列头指针 *p 所指向的任务不是当前任务时，说明
 // 在本任务插入等待队列后还有任务进入等待队列。于是我们应该也要唤醒这个任务，而我
 // 们自己应按顺序让这些后面进入队列的任务唤醒，因此这里将等待队列头所指任务先置为
 // 就绪状态，而自己则置为不可中断等待状态，即自己要等待这些后续进队列的任务被唤醒
 // 而执行时来唤醒本任务。然后重新执行调度程序。
	if (*p && *p != current) {
		(**p).state = 0;
		current->state = TASK_UNINTERRUPTIBLE;
		goto repeat;
	}
	// 执行到这里，说明本任务真正被唤醒执行。此时等待队列头指针应该指向本任务，若它为
 	// 空，则表明调度有问题，于是显示警告信息。最后我们让头指针指向在我们前面进入队列
 	// 的任务（*p = tmp）。 若确实存在这样一个任务，即队列中还有任务（tmp 不为空），就
 	// 唤醒之。最先进入队列的任务在唤醒后运行时最终会把等待队列头指针置成 NULL。

	if (!*p)
		printk("Warning: *P = NULL\n\r");
	if (*p = tmp)
		tmp->state=0;
}
// 将当前任务置为可中断的等待状态（TASK_INTERRUPTIBLE），并放入头指针*p 指定的等待
 // 队列中。

void interruptible_sleep_on(struct task_struct **p)
{
	__sleep_on(p,TASK_INTERRUPTIBLE);
}
// 把当前任务置为不可中断的等待状态（TASK_UNINTERRUPTIBLE），并让睡眠队列头指针指向
 // 当前任务。只有明确地唤醒时才会返回。该函数提供了进程与中断处理程序之间的同步机制。
void sleep_on(struct task_struct **p)
{
	__sleep_on(p,TASK_UNINTERRUPTIBLE);
}
 // 唤醒 *p 指向的任务。*p 是任务等待队列头指针。由于新等待任务是插入在等待队列头指针
 // 处的，因此唤醒的是最后进入等待队列的任务。若该任务已经处于停止或僵死状态，则显示
 // 警告信息。
void wake_up(struct task_struct **p)
{
	if (p && *p) {
		if ((**p).state == TASK_STOPPED)// 处于停止状态。
			printk("wake_up: TASK_STOPPED");
		if ((**p).state == TASK_ZOMBIE)// 处于僵死状态。
			printk("wake_up: TASK_ZOMBIE");
		(**p).state=0;// 置为就绪状态 TASK_RUNNING。
	}
}

/*
 * OK, here are some floppy things that shouldn't be in the kernel
 * proper. They are here because the floppy needs a timer, and this
 * was the easiest way of doing it.
 */
// 下面代码用于处理软驱定时。
static struct task_struct * wait_motor[4] = {NULL,NULL,NULL,NULL};
static int  mon_timer[4]={0,0,0,0};
static int moff_timer[4]={0,0,0,0};
unsigned char current_DOR = 0x0C;

int ticks_to_floppy_on(unsigned int nr)
{
	extern unsigned char selected;
	unsigned char mask = 0x10 << nr;

	if (nr>3)
		panic("floppy_on: nr>3");
	moff_timer[nr]=10000;		/* 100 s = very big :-) */
	cli();				/* use floppy_off to turn it off */
	mask |= current_DOR;
	if (!selected) {
		mask &= 0xFC;
		mask |= nr;
	}
	if (mask != current_DOR) {
		outb(mask,FD_DOR);
		if ((mask ^ current_DOR) & 0xf0)
			mon_timer[nr] = HZ/2;
		else if (mon_timer[nr] < 2)
			mon_timer[nr] = 2;
		current_DOR = mask;
	}
	sti();
	return mon_timer[nr];
}

void floppy_on(unsigned int nr)
{
	cli();
	while (ticks_to_floppy_on(nr))
		sleep_on(nr+wait_motor);
	sti();
}

void floppy_off(unsigned int nr)
{
	moff_timer[nr]=3*HZ;
}

void do_floppy_timer(void)
{
	int i;
	unsigned char mask = 0x10;

	for (i=0 ; i<4 ; i++,mask <<= 1) {
		if (!(mask & current_DOR))
			continue;
		if (mon_timer[i]) {
			if (!--mon_timer[i])
				wake_up(i+wait_motor);
		} else if (!moff_timer[i]) {
			current_DOR &= ~mask;
			outb(current_DOR,FD_DOR);
		} else
			moff_timer[i]--;
	}
}

// 下面是关于定时器的代码。最多可有 64 个定时器。
#define TIME_REQUESTS 64
// 定时器链表结构和定时器数组。该定时器链表专用于供软驱关闭马达和启动马达定时操作。
 // 这种类型定时器类似现代 Linux 系统中的动态定时器（Dynamic Timer），仅供内核使用。
static struct timer_list {
	long jiffies;
	void (*fn)();// 定时处理程序。
	struct timer_list * next;
} timer_list[TIME_REQUESTS], * next_timer = NULL;// next_timer 是定时器队列头指针。

// 添加定时器。输入参数为指定的定时值(滴答数)和相应的处理程序指针。
void add_timer(long jiffies, void (*fn)(void))
{
	struct timer_list * p;

	// 如果定时处理程序指针为空，则退出。否则关中断。
	if (!fn)
		return;
	cli();
	// 如果定时值<=0，则立刻调用其处理程序。并且该定时器不加入链表中。
	if (jiffies <= 0)
		(fn)();
	else {
		// 否则从定时器数组中，找一个空闲项。
		for (p = timer_list ; p < timer_list + TIME_REQUESTS ; p++)
			if (!p->fn)
				break;

		// 如果已经用完了定时器数组，则系统崩溃，否则向定时器数据结构填入相应信息，并链入
 		// 链表头。
		if (p >= timer_list + TIME_REQUESTS)
			panic("No more time requests free");
		p->fn = fn;
		p->jiffies = jiffies;
		p->next = next_timer;
		next_timer = p;

		// 链表项按定时值从小到大排序。在排序时减去排在前面需要的滴答数，这样在处理定时器时
 		// 只要查看链表头的第一项的定时是否到期即可。这段程序好象没有考虑周全。如果新
 		// 插入的定时器值小于原来头一个定时器值时则根本不会进入循环中，但此时还是应该将紧随
 		// 其后面的一个定时器值减去新的第 1 个的定时值。即如果第 1 个定时值<=第 2 个，则第 2 个
 		// 定时值扣除第 1 个的值即可，否则进入下面循环中进行处理。
		while (p->next && p->next->jiffies < p->jiffies) {
			p->jiffies -= p->next->jiffies;
			fn = p->fn;
			p->fn = p->next->fn;
			p->next->fn = fn;
			jiffies = p->jiffies;
			p->jiffies = p->next->jiffies;
			p->next->jiffies = jiffies;
			p = p->next;
		}
	}
	sti();
}

// 时钟中断 C 函数处理程序
// 参数 cpl 是当前特权级 0 或 3，是时钟中断发生时正被执行的代码选择符中的特权级。
// cpl=0 时表示中断发生时正在执行内核代码；cpl=3 时表示中断发生时正在执行用户代码。
// 对于一个进程由于执行时间片用完时，则进行任务切换。并执行一个计时更新工作。
void do_timer(long cpl)
{
	static int blanked = 0;
	// 首先判断是否经过了一定时间而让屏幕黑屏（blankout）。如果 blankcount 计数不为零，
 	// 或者黑屏延时间隔时间 blankinterval 为 0 的话，那么若已经处于黑屏状态（黑屏标志
 	// blanked = 1），则让屏幕恢复显示。若 blankcount 计数不为零，则递减之，并且复位
 	// 黑屏标志。
	if (blankcount || !blankinterval) {
		if (blanked)
			unblank_screen();
		if (blankcount)
			blankcount--;
		blanked = 0;
		// 否则的话若黑屏标志未置位，则让屏幕黑屏，并且设置黑屏标志。
	} else if (!blanked) {
		blank_screen();
		blanked = 1;
	}
	// 接着处理硬盘操作超时问题。如果硬盘超时计数递减之后为 0，则进行硬盘访问超时处理。
	if (hd_timeout)
		if (!--hd_timeout)
			hd_times_out();
	// 如果发声计数次数到，则关闭发声。
	if (beepcount)
		if (!--beepcount)
			sysbeepstop();
	// 如果当前特权级(cpl)为 0（最高，表示是内核程序在工作），则将内核代码运行时间 stime
 	// 递增；如果 cpl > 0，则表示是一般用户程序在工作，增加 utime。
	if (cpl)
		current->utime++;
	else
		current->stime++;

	// 如果有定时器存在，则将链表第 1 个定时器的值减 1。如果已等于 0，则调用相应的处理程序，
 	// 并将该处理程序指针置为空。然后去掉该项定时器。next_timer 是定时器链表的头指针。
	if (next_timer) {
		next_timer->jiffies--;
		while (next_timer && next_timer->jiffies <= 0) {
			void (*fn)(void);
			
			fn = next_timer->fn;
			next_timer->fn = NULL;
			next_timer = next_timer->next;
			(fn)();// 调用定时处理函数。
		}
	}
	if (current_DOR & 0xf0)
		do_floppy_timer();
	// 如果进程运行时间还没完，则退出。否则置当前任务运行计数值为 0。并且若发生时钟中断时
 	// 正在内核代码中运行则返回，否则调用执行调度函数。
	if ((--current->counter)>0) return;
	current->counter=0;
	if (!cpl) return;// 对于内核态程序，不依赖 counter 值进行调度。
	schedule();
}

// 系统调用功能 - 设置报警定时时间值(秒)。
 // 若参数 seconds 大于 0，则设置新定时值，并返回原定时时刻还剩余的间隔时间。否则返回 0。
 // 进程数据结构中报警定时值 alarm 的单位是系统滴答（1 滴答为 10 毫秒），它是系统开机起到
 // 设置定时操作时系统滴答值 jiffies 和转换成滴答单位的定时值之和，即'jiffies + HZ*定时
 // 秒值'。而参数给出的是以秒为单位的定时值，因此本函数的主要操作是进行两种单位的转换。
 // 其中常数 HZ = 100，是内核系统运行频率。参数 seconds 是新的定时时间值，单位是秒。
int sys_alarm(long seconds)
{
	int old = current->alarm;

	if (old)
		old = (old - jiffies) / HZ;
	current->alarm = (seconds>0)?(jiffies+HZ*seconds):0;
	return (old);
}
// 取当前进程号 pid
int sys_getpid(void)
{
	return current->pid;
}
// 取父进程号 ppid
int sys_getppid(void)
{
	return current->p_pptr->pid;
}
// 取用户号 uid
int sys_getuid(void)
{
	return current->uid;
}
// 取有效的用户号 euid
int sys_geteuid(void)
{
	return current->euid;
}
// 取组号 gid。
int sys_getgid(void)
{
	return current->gid;
}
// 取有效的组号 egid。
int sys_getegid(void)
{
	return current->egid;
}
// 系统调用功能 -- 降低对 CPU 的使用优先权
int sys_nice(long increment)
{
	if (current->priority-increment>0)
		current->priority -= increment;
	return 0;
}
// 内核调度程序的初始化子程序
void sched_init(void)
{
	int i;
	struct desc_struct * p;// 描述符表结构指针。

	if (sizeof(struct sigaction) != 16)
		panic("Struct sigaction MUST be 16 bytes");
	// 在全局描述符表中设置初始任务（任务 0）的任务状态段描述符和局部数据表描述符。
	set_tss_desc(gdt+FIRST_TSS_ENTRY,&(init_task.task.tss));
	set_ldt_desc(gdt+FIRST_LDT_ENTRY,&(init_task.task.ldt));
	// 清任务数组和描述符表项（注意 i=1 开始，所以初始任务的描述符还在）。
	p = gdt+2+FIRST_TSS_ENTRY;
	for(i=1;i<NR_TASKS;i++) {
		task[i] = NULL;
		p->a=p->b=0;
		p++;
		p->a=p->b=0;
		p++;
	}
/* Clear NT, so that we won't have troubles with that later on */
// EFLAGS 中的 NT 标志位用于控制任务的嵌套调用。当 NT 位置位时，那么当前中断任务执行
// IRET 指令时就会引起任务切换。NT 指出 TSS 中的 back_link 字段是否有效。NT=0 时无效。
	__asm__("pushfl ; andl $0xffffbfff,(%esp) ; popfl");
	ltr(0);
	lldt(0);
	// 下面代码用于初始化 8253 定时器。通道 0，选择工作方式 3，二进制计数方式。通道 0 的
 	// 输出引脚接在中断控制主芯片的 IRQ0 上，它每 10 毫秒发出一个 IRQ0 请求。LATCH 是初始
 	// 定时计数值。
	outb_p(0x36,0x43);		/* binary, mode 3, LSB/MSB, ch 0 */
	outb_p(LATCH & 0xff , 0x40);	/* LSB */
	outb(LATCH >> 8 , 0x40);	/* MSB */
	// 设置时钟中断处理程序句柄（设置时钟中断门）。修改中断控制器屏蔽码，允许时钟中断。
 	// 然后设置系统调用中断门。
	set_intr_gate(0x20,&timer_interrupt);
	outb(inb_p(0x21)&~0x01,0x21);
	set_system_gate(0x80,&system_call);
}
