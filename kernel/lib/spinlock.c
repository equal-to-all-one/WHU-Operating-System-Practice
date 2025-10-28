#include "lib/lock.h"
#include "lib/print.h"
#include "proc/cpu.h"
#include "riscv.h"

// 带层数叠加的关中断
void push_off(void)
{
    int old = intr_get();
    
    // disable interrupts to prevent an involuntary（非自愿的） context
    // switch（e.g 时间片调度，外部中断） while using mycpu().
    intr_off();
    
    // 如果是第一次调用push_off， 保存当前状态
    if(mycpu()->noff == 0)
        mycpu()->origin = old;
    // 增加嵌套计数
    mycpu()->noff += 1;
}

// 带层数叠加的开中断
void pop_off(void)
{
    // 此时在push_off中已经关闭了中断
    struct cpu *c = mycpu();
    // 若push_off/ pop_off 是配对使用的，则中断在此期间不应被打开
    if(intr_get())
        panic("pop_off - interruptible");
    // 若push_off/ pop_off 是配对使用的，则中断在此期间不应 < 1
    if(c->noff < 1)
        panic("pop_off");
    // 减少嵌套计数
    c->noff -= 1;
    // 若嵌套计数为0，且之前中断是打开的，则重新打开中断
    if(c->noff == 0 && c->origin)
        intr_on();
}

// 是否持有自旋锁
// 中断应当是关闭的
bool spinlock_holding(spinlock_t *lk)
{
    int holding;
    holding = (lk->locked && lk->cpuid == mycpuid());
    return holding;
}

// 自选锁初始化
void spinlock_init(spinlock_t *lk, char *name)
{
    lk->name = name;
    lk->locked = 0; // 没有被持有
    lk->cpuid = 0; // 默认值（无意义）
}

// 获取自选锁
void spinlock_acquire(spinlock_t *lk)
{    
    /*
        当获取锁后再临界区中执行时，中断有可能到来，如果该中断也要
        获取该锁，由于锁被当前cpu持有，所以中断处理程序会一直spin（自旋）
        导致死锁。
    */
    push_off(); // disable interrupts to avoid deadlock.
    
    // 若当前cpu重复获取锁，会导致死锁
    if(spinlock_holding(lk))
        panic("acquire spinlock");

    // On RISC-V, sync_lock_test_and_set turns into an atomic swap:
    //   a5 = 1
    //   s1 = &lk->locked
    //   amoswap.w.aq a5, a5, (s1)
    
    // RISC-V的原子内存操作（AMO）指令通用格式为：
    // amo<op>.<size>.<ordering> rd, rs2, (rs1)
    // rd: 目标寄存器（Destination Register），用于存放从内存中读取出的原始值。
    // rs2: 源寄存器（Source Register），存放要与内存值进行运算或交换的新值。
    // (rs1): 内存地址，由源寄存器 rs1 的值指定。
    // amo : Atomic Memory Operation; .w : word(32bit); 
    // aq : acquire Acquire语义是一种内存屏障（Memory Fence），它规定：
    // 在此条 aq 指令之后的所有内存访问（读或写），都不能被处理器重排到此条指令之前执行。
    // 与之对应的是 rl (Release，释放) 语义，用于释放锁。rl 确保在它之前的所有内存读写操作
    // 都必须完成，才能执行 rl 指令

    // sync：同步原语
    while(__sync_lock_test_and_set(&lk->locked, 1) != 0)
        ;

    // Tell the C compiler and the processor to not move loads or stores
    // past（跨过） this point, to ensure that the critical section's memory
    // references happen strictly after the lock is acquired.
    // On RISC-V, this emits a fence instruction.
    __sync_synchronize();

    // Record info about lock acquisition for holding() and debugging.
    lk->cpuid = mycpuid();
} 

// 释放自旋锁
void spinlock_release(spinlock_t *lk)
{
    // 只有持有锁时才能释放锁
    if(!spinlock_holding(lk))
        panic("release");

    // Clear the lock holder.
    lk->cpuid = 0;

    // Tell the C compiler and the CPU to not move loads or stores
    // past（跨过） this point, to ensure that all the stores in the critical
    // section are visible to other CPUs before the lock is released,
    // and that loads in the critical section occur strictly before
    // the lock is released.
    // On RISC-V, this emits a fence instruction.

    // 和 acquire() 中的 __sync_synchronize() 一起，保证了临界区的内存访问指令
    // 的位置，不会因为编译器的优化而导致指令重排，从而确保临界区的正确性。
    __sync_synchronize();

    // Release the lock, equivalent to lk->locked = 0.
    // This code doesn't use a C assignment, since the C standard
    // implies that an assignment might be implemented with
    // multiple store instructions.
    // On RISC-V, sync_lock_release turns into an atomic swap:
    //   s1 = &lk->locked
    //   amoswap.w.rl zero, zero, (s1)
    __sync_lock_release(&lk->locked);

    // 打开在acquire()中关闭的中断
    pop_off();
}