/*
 *  linux/kernel/fork.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 *  'fork.c' contains the help-routines for the 'fork' system call
 * (see also system_call.s), and some misc functions ('verify_area').
 * Fork is rather simple, once you get the hang of it, but the memory
 * management can be a bitch. See 'mm/mm.c': 'copy_page_tables()'
 */
#include <errno.h>           // 错误号头文件。包含系统中各种出错号。

#include <linux/sched.h>    // 调度程序头文件，定义了任务结构 task_struct、任务 0 的数据。
#include <linux/kernel.h>  // 内核头文件。含有一些内核常用函数的原形定义。
#include <asm/segment.h> // 段操作头文件。定义了有关段寄存器操作的嵌入式汇编函数。
#include <asm/system.h> // 系统头文件。定义了设置或修改描述符/中断门等的嵌入式汇编宏。

extern void write_verify(unsigned long address); // 写页面验证。若页面不可写，则复制页面。

long last_pid=0; // 最新进程号，其值会由 get_empty_process()生成。


 /*
    @description: 进程空间区域写前验证函数。

    该函数对当前进程逻辑地址从 addr 到 addr + size 这一段范围以页为单位执行写操作前的
	检测操作。由于检测判断是以页面为单位进行操作，因此程序首先需要找出 addr 所在页面
	开始地址 start，然后 start 加上进程数据段基址，使这个 start 变换成 CPU 4G 线性
	空间中的地址。最后循环调用 write_verify() 对指定大小的内存空间进行写前验证。若页面是
	只读的，则执行共享检验和复制页面操作（写时复制）。 
 */


void verify_area(void * addr,int size)
{
	unsigned long start;

	start = (unsigned long) addr;
	size += start & 0xfff;
	start &= 0xfffff000; // 此时 start 是当前进程空间中的逻辑地址。
	start += get_base(current->ldt[2]);
	while (size>0) {
		size -= 4096;
		write_verify(start);
		start += 4096;
	}
}


 /*
    @description: 复制内存页表。

    参数 nr 是新任务号；p 是新任务数据结构指针。该函数为新任务在线性地址空间中设置代码段和
	数据段基址、限长，并复制页表。 由于 Linux 系统采用了写时复制（copy on write）技术，
	因此这里仅为新进程设置自己的页目录表项和页表项，而没有实际为新进程分配物理内存页面。
	此时新进程与其父进程共享所有内存页面。操作成功返回 0，否则返回出错号。
 */
int copy_mem(int nr,struct task_struct * p)
{
	unsigned long old_data_base,new_data_base,data_limit;
	unsigned long old_code_base,new_code_base,code_limit;

	code_limit=get_limit(0x0f); //取当前进程局部描述符表中代码段描述符项中的段限长
	data_limit=get_limit(0x17); //数据段描述符
	old_code_base = get_base(current->ldt[1]); //代码段在线性地址空间中的基地址。
	old_data_base = get_base(current->ldt[2]);//数据段
	if (old_data_base != old_code_base) // 检查代码段和数据段基址是否相同
		panic("We don't support separate I&D"); //内核显示出错信息，停止运行
	if (data_limit < code_limit) //数据段的长度不小于代码段的长度
		panic("Bad data_limit");
	new_data_base = new_code_base = nr * TASK_SIZE; //设置创建中的新进程在线性地址空间中的基地址等于（64MB * 其任务号）
	p->start_code = new_code_base; //设置新进程局部描述符表中段描述符中的基地址。
	set_base(p->ldt[1],new_code_base); //复制当前进程（父进程）的页目录表项和页表项。
	set_base(p->ldt[2],new_data_base); //此时子进程共享父进程的内存页面
	if (copy_page_tables(old_data_base,new_data_base,data_limit)) { //若出错
		free_page_tables(new_data_base,data_limit); //释放刚申请的页表项。
		return -ENOMEM;
	}
	return 0;
}

/*
 *  Ok, this is the main fork-routine. It copies the system process
 * information (task[nr]) and sets up the necessary registers. It
 * also copies the data segment in it's entirety.
 */

 /*
    @description:  复制进程

	该函数的参数是进入系统调用中断处理过程（sys_call.s）开始，直到调用本系统调用处理过程和调用本函数前
	逐步压入进程内核态栈的各寄存器的值。这些在 sys_call.s 程序中逐步压入内核态栈的值（参数）包括:
	1) CPU 执行中断指令压入的用户栈地址 ss 和 esp、标志 eflags 和返回地址 cs 和 eip；
	2) 在刚进入 system_call 时入栈的段寄存器 ds、es、fs 和 edx、ecx、edx；
	3) 调用 sys_call_table 中 sys_fork 函数时入栈的返回地址（参数 none 表示）；
	4) 调用 copy_process()之前入栈的 gs、esi、edi、ebp 和 eax（nr）。
	其中参数 nr 是调用 find_empty_process()分配的任务数组项号。
 */

int copy_process(int nr,long ebp,long edi,long esi,long gs,long none,
		long ebx,long ecx,long edx, long orig_eax, 
		long fs,long es,long ds,
		long eip,long cs,long eflags,long esp,long ss)
{
	struct task_struct *p;
	int i;
	struct file *f;
/* 
	首先为新任务数据结构分配内存。如果内存分配出错，则返回出错码并退出。然后将新任务
	结构指针放入任务数组的 nr 项中。其中 nr 为任务号，由前面 find_empty_process()返回。
	接着把当前进程任务结构内容复制到刚申请到的内存页面 p 开始处。
*/
	p = (struct task_struct *) get_free_page();
	if (!p)
		return -EAGAIN;
	task[nr] = p;
	*p = *current;	/* NOTE! this doesn't copy the supervisor stack */
/* 
	随后对复制来的进程结构内容进行一些修改，作为新进程的任务结构。先将新进程的状态
	置为不可中断等待状态，以防止内核调度其执行。然后设置新进程的进程号 pid，并初始化
	进程运行时间片值等于其 priority 值。接着复位新进程的信号位图、报警定时值、
	会话（session）领导标志 leader、 进程及其子进程在内核和用户态运行时间统计值，
	还设置进程开始运行的系统时间 start_time。
*/
	p->state = TASK_UNINTERRUPTIBLE;
	p->pid = last_pid;
	p->counter = p->priority;
	p->signal = 0;
	p->alarm = 0;
	p->leader = 0;		/* process leadership doesn't inherit */
	p->utime = p->stime = 0;
	p->cutime = p->cstime = 0;
	p->start_time = jiffies;
/*
	再修改任务状态段 TSS 数据。由于系统给任务结构 p 分配了 1 页新内存，所以 
	(PAGE_SIZE + (long) p) 让 esp0 正好指向该页顶端。 ss0:esp0 用作程序在内核态
	执行时的栈。另外由于每个任务在 GDT 表中都有两个段描述符: TSS 段描述符和 LDT
	表段描述符。函数会把 GDT 中本任务 LDT 段描述符的选择符保存在本任务的 TSS 段中。
	当 CPU 执行切换任务时，会自动从 TSS 中把 LDT 段描述符的选择符加载到 ldtr 寄存器中。
*/
	p->tss.back_link = 0;
	p->tss.esp0 = PAGE_SIZE + (long) p;
	p->tss.ss0 = 0x10;
	p->tss.eip = eip;
	p->tss.eflags = eflags;
	p->tss.eax = 0;
	p->tss.ecx = ecx;
	p->tss.edx = edx;
	p->tss.ebx = ebx;
	p->tss.esp = esp;
	p->tss.ebp = ebp;
	p->tss.esi = esi;
	p->tss.edi = edi;
	p->tss.es = es & 0xffff;
	p->tss.cs = cs & 0xffff;
	p->tss.ss = ss & 0xffff;
	p->tss.ds = ds & 0xffff;
	p->tss.fs = fs & 0xffff;
	p->tss.gs = gs & 0xffff;
	p->tss.ldt = _LDT(nr);
	p->tss.trace_bitmap = 0x80000000;
	if (last_task_used_math == current) //如果当前任务使用了协处理器，就保存其上下文。
		__asm__("clts ; fnsave %0 ; frstor %0"::"m" (p->tss.i387));
/*
	接下来复制进程页表。即在线性地址空间中设置新任务代码段和数据段描述符中的基址和限长，
	并复制页表。如果出错（返回值不是 0），则复位任务数组中相应项并释放为该新任务分配的
	用于任务结构的内存页。
*/
	if (copy_mem(nr,p)) {
		task[nr] = NULL;
		free_page((long) p);
		return -EAGAIN;
	}
/*
	如果父进程中有文件是打开的，则将对应文件的打开次数增 1。因为这里创建的子进程
	会与父进程共享这些打开的文件。将当前进程（父进程）的 pwd, root 和 executable
	引用次数均增 1。与上面同样的道理，子进程也引用了这些 i 节点。
*/
	for (i=0; i<NR_OPEN;i++)
		if (f=p->filp[i])
			f->f_count++;
	if (current->pwd)
		current->pwd->i_count++;
	if (current->root)
		current->root->i_count++;
	if (current->executable)
		current->executable->i_count++;
	if (current->library)
		current->library->i_count++;
/*
	随后在 GDT 表中设置新任务 TSS 段和 LDT 段描述符项。这两个段的限长均被设置成 104
	字节。然后设置进程之间的关系链表指针，即把新进程插入到当前进程的子进程链表中。
	把新进程的父进程设置为当前进程，把新进程的最新子进程指针 p_cptr 和年轻兄弟进程指针
	p_ysptr 置空。接着让新进程的老兄进程指针 p_osptr 设置等于父进程的最新子进程指针。
	若当前进程却是还有其他子进程，则让比邻老兄进程的最年轻进程指针 p_yspter 指向新进程。
	最后把当前进程的最新子进程指针指向这个新进程。然后把新进程设置成就绪态。最后返回新进程号。
	“gdt+(nr<<1)+FIRST_TSS_ENTRY”是任务 nr 的 TSS 描述符项在全局表中的地址。因为每个任务
	占用 GDT 表中 2 项，所以上式中要包括'(nr<<1)'。在任务切换时，任务寄存器 tr 会由 CPU 自动加载。
*/
	set_tss_desc(gdt+(nr<<1)+FIRST_TSS_ENTRY,&(p->tss));
	set_ldt_desc(gdt+(nr<<1)+FIRST_LDT_ENTRY,&(p->ldt));
	p->p_pptr = current;
	p->p_cptr = 0;
	p->p_ysptr = 0;
	p->p_osptr = current->p_cptr;
	if (p->p_osptr)
		p->p_osptr->p_ysptr = p;
	current->p_cptr = p;
	p->state = TASK_RUNNING;	/* do this last, just in case */
	return last_pid;
}

/*
    @description:   为新进程取得不重复的进程号 last_pid

	函数返回在任务数组中的任务号(数组项)。
 */

int find_empty_process(void)
{
	int i;

	repeat:
		if ((++last_pid)<0) last_pid=1;
		for(i=0 ; i<NR_TASKS ; i++)
			if (task[i] && ((task[i]->pid == last_pid) ||
				        (task[i]->pgrp == last_pid)))
				goto repeat;
	for(i=1 ; i<NR_TASKS ; i++)
		if (!task[i]) //排除0
			return i;
	return -EAGAIN;
}
