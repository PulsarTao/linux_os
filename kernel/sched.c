#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/sys.h>
#include <linux/fdreg.h>
#include <asm/system.h>
#include <asm/io.h>
#include <asm/segment.h>

#include <signal.h>

//取信號二進制數值 ex： 信號5   1<<(5-1) = 16 = 00010000b
#define _S(nr) (1<<(nr-1))

//除SIGKILL SIGSTOP都可以阻塞
#define _BLOCKABLE (~(_S(SIGKILL) | _S(SIGSTOP)))

//内核調試函數。顯示任務號nr的進程號，進程狀態和内核堆棧空閑字節數
void show_task(int nr, struct task_struct * p)
{
	int i, j = 4096-sizeof(struct task_struct);
	printk("%d: pid=%d, state=%d, ",nr, p->pid, p->state);
	i = 0;
	while (i < j && !((char *)(p+1))[i]) //檢測指定任務數據結構以後等於0的字節數
		i++;
	printk("%d (of %d) chars free in kernel stack\n\r",i, j);
}

//顯示所有任務的任務號，進程號，進程狀態和内核堆棧空閑字節數
void show_stat(void)
{
	int i;
	for (i = 0; i < NR_TASKS; i++)
		if (task[i])
			show_task(i, task[i]);
}

//PC機8253定時芯片的輸入時鐘頻率約爲1.193180MHZ.set成100HZ
#define LATCH (1193180/HZ)

extern void mem_use(void);

extern int timer_interrupt(void);
extern int system_call(void);

//每個任務在内核運行時都有自己的内核態堆棧。定義任務内核態堆棧結構
union task_union {
	struct task_struct task; //一個任務的數據結構與其内核態堆棧
	char stack[PAGE_SIZE]; //在同一頁，故從堆棧段寄存器ss可獲取其數據段選擇符
};

static union task_union init_task = {INIT_TASK,};//定義初始任務數據

long volatile jiffies = 0;
long startup_time = 0;

struct task_struct *current = &(init_task.task); 
struct task_struct *last_task_used_math = NULL; //使用過協處理器的任務指針

struct task_struct *task[NR_TASKS] = {&(init_task.task), };

//定義用戶堆棧 1K項 容量4K字節  内核初始化用於内核棧
//初始化完成用於任務0，1用戶態堆棧
long user_stack [ PAGE_SIZE>>2 ];

struct {
	long * a;
	short b;
} stack_start = { & user_stack [PAGE_SIZE>>2], 0x10 };

/*將當前協處理器内容保存至老協處理器狀態數組中。并將當前任務的協處理器
	内容加載到協處理器
 */
void math_state_restore()
{
	if (last_task_used_math == current)
		return ;
	__asm__("fwait");
	if (last_task_used_math) {
		__asm__("fnsave %0"::"m" (last_task_used_math->tss.i387));
	}
	last_task_used_math = current;
	if (current->used_math) {
		__asm__("frstor %0"::"m" (current->tss.i387));
	} else {
		__asm__("fninit"::);
		current->used_math = 1;
	}
}

/*任務0是閑置任務。只有沒有其他任務運行時才被調用*/
void schedule(void)
{
	int i, next, c;
	struct task_struct **p;
//檢測alarm，喚醒任何已得到信號的可中斷任務
	for (p = &LAST_TASK; p > &FIRST_TASK ; --p)
		if (*p) {
			if ((*p)->alarm && (*p)->alarm < jiffies) {
				(*p)->signal |= (1<<(SIGALRM-1));
				(*p)->alarm = 0;
			}
			if (((*p)->signal & ~(_BLOCKABLE & (*p)->blocked)) &&
					(*p)->state == TASK_INTERRUPTIBLE)
				(*p)->state = TASK_RUNNING;
		}
	while (1) {
		c = -1;
		next = 0;
		i = NR_TASKS;
		p = &task[NR_TASKS];
		while (--i) {
			if (!*--p)
				continue;
			if ((*p)->state == TASK_RUNNING && (*p)->counter > c)
				c = (*p)->counter, next = i;
		}
		if (c) break;
		for (p = &LAST_TASK; p > &FIRST_TASK; --p)
			if (*p)
				(*p)->counter = ((*p)->counter >> 1) +
									(*p)->priority;
	}
	switch_to(next);
}


//轉換當前任務的狀態爲可中斷的等待狀態，並重新調度
//未完全實現
int sys_pause(void)
{
	current->state = TASK_INTERRUPTIBLE;
	schedule();
	return 0;
}

//把當前任務置爲不可中斷的等待狀態，並讓睡眠隊列頭指針指向當前任務
//只有明確的喚醒時才返回
void sleep_on(struct task_struct **p)
{
	struct task_struct *tmp;
	if (!p)
		return ;
	if (current == &(init_task.task))
		panic("task[0] trying to sleep");
	tmp = *p;
	*p = current;
	current->state = TASK_UNINTERRUPTIBLE;
	schedule();
	if (tmp)
		tmp->state = 0;
}

//將當前任務置為可中斷等待狀態，並放入*p指定的等待隊列中
void interruptible_sleep_on(struct task_struct **p)
{
	struct task_struct *tmp;
	if (!p)
		return ;
	if (current == &(init_task.task))
		panic("task[0] trying to sleep");
	tmp = *p;
	*p  = current;
repeat:
	current->state = TASK_INTERRUPTIBLE;
	schedule();
	if (*p && *p != current) {
		(**p).state = 0;
		goto repeat;
	}
	*p = NULL;
	if (tmp)
		tmp->state = 0;
}


void wake_up(struct task_struct **p)
{
	if (p && *p) {
		(**p).state = 0;
		*p = NULL;
	}
}


/*floppy intr*/
//等待馬達正常啓動進程指針。0-3對應軟驅A-D
static struct task_struct * wait_motor[4] = {NULL,NULL,NULL,NULL};
//存放各軟驅馬達所需要滴答數
static int mon_timer[4] = {0,0,0,0};
//馬達停轉之前需維持時間
static int moff_timer[4] = {0,0,0,0};

//對應軟驅控制器當前輸出寄存器 7-4未使用
//位3 1允許DMA中斷 0-禁止DMA中斷請求
//2   1 啓動軟盤控制器  ，0復位軟盤控制器
//1-0 00-11 選擇控制器A-D
unsigned char current_DOR = 0x0C;//允許DMA和中斷請求啓動FDC
int ticks_to_floppy_on(unsigned int nr)
{
	extern unsigned char selected;
	unsigned char mask = 0x10 << nr;
	if (nr > 3)
		panic("floppy_on: nr>3");
	moff_timer[nr] = 10000; //100s
	cli();
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
	moff_timer[nr] = 3*HZ;
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

//定時器 64個
#define TIME_REQUEST 64
static struct timer_list {
	long jiffies;
	void (*fn)();
	struct timer_list * next;
} timer_list[TIME_REQUEST], *next_timer = NULL;

void add_timer(long jiffies, void (*fn)(void))
{
	struct timer_list * p;
	if (!fn)
		return;
	cli();
	if (jiffies <= 0)
		(fn)();
	else {
		for (p = timer_list ; p < timer_list + TIME_REQUEST; p++)
			if (!p->fn)
				break;
		if (p >= timer_list + TIME_REQUEST)
			panic("No more time request free");
		p->fn = fn;
		p->jiffies = jiffies;
		p->next = next_timer;
		next_timer = p;
		while (p->next && p->next->jiffies < p->jiffies) {
			p->jiffies -= p->next->jiffies;
			fn = p->fn;
			p->fn = p->next->fn;
			p->next->fn = fn;
			jiffies = p->jiffies;
			p->next->jiffies = jiffies;
			p = p->next;
		}
	}
	sti();
}



void do_timer()
{}

void sched_init(void)
{}











































