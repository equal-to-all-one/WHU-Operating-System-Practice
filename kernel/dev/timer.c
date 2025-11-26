#include "lib/lock.h"
#include "lib/print.h"
#include "dev/timer.h"
#include "memlayout.h"
#include "proc/cpu.h"
#include "riscv.h"

/*-------------------- 工作在M-mode --------------------*/

// in trap.S M-mode时钟中断处理流程()
extern void timer_vector();

// 每个CPU在时钟中断中需要的临时空间(考虑为什么可以这么写)
static uint64 mscratch[NCPU][5];

// 时钟初始化
// called in start.c
void timer_init()
{

    // 获取当前hartid
    uint64 id = r_mhartid();

    // 1. 设置mscratch，供timer_vector使用
    // mscratch[0..2] are for saving registers a1, a2, a3
    // mscratch[3] = address of CLINT_MTIMECMP(id)
    // mscratch[4] = INTERVAL
    mscratch[id][3] = CLINT_MTIMECMP(id);
    mscratch[id][4] = INTERVAL;
    w_mscratch((uint64)mscratch[id]);

    // 2. 设置第一次时钟中断触发时间
    // *MTIMECMP = *MTIME + INTERVAL

    // mtime从模拟器启动时就开始计时了。当你运行到 timer_init 时，MTIME 
    // 可能已经是一个比较大的值了（比如 1,000,000），而 MTIMECMP 如果不初始化，
    // 它的默认值通常是 0
    // 这会导致第一次时钟中断会立即触发，而且连续不断的触发而导致系统无法正常运行
    // 因为MTIME在定时器中断启动时值已经很大了
    *(uint64*)CLINT_MTIMECMP(id) = *(uint64*)CLINT_MTIME + INTERVAL;

    // 3. 设置mtvec跳转地址
    // 什么时候使用 : 每次时钟中断发生时硬件自动跳转到mtvec指向的地址
    w_mtvec((uint64)timer_vector);

    // 4. 开启M-mode时钟中断
    // 设置mie寄存器的MTIE位
    w_mie(r_mie() | MIE_MTIE);

    // 5. 开启全局M-mode中断
    // M-mode（机器模式）下的中断默认是关闭的
    // 设置mstatus寄存器的MIE位
    w_mstatus(r_mstatus() | MSTATUS_MIE);
}


/*--------------------- 工作在S-mode --------------------*/

// 系统时钟
static timer_t sys_timer;

// 时钟创建(初始化系统时钟)
void timer_create()
{
    sys_timer.ticks = 0;
    spinlock_init(&sys_timer.lk, "timer");
}

// 时钟更新(ticks++ with lock)
void timer_update()
{
    // 一个cpu负责更新系统时钟即可
    if (mycpuid() == 0) {
        spinlock_acquire(&sys_timer.lk);
        sys_timer.ticks++;
        // 每100个ticks打印一次 调试信息
        // if (sys_timer.ticks % 100 == 0) {
        //     printf("ticks: %d\n", sys_timer.ticks);
        // }
        spinlock_release(&sys_timer.lk);
    }
}

// 返回系统时钟ticks
uint64 timer_get_ticks()
{
    uint64 ret;
    spinlock_acquire(&sys_timer.lk);
    ret = sys_timer.ticks;
    spinlock_release(&sys_timer.lk);
    return ret;
}