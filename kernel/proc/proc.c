#include "lib/print.h"
#include "lib/str.h"
#include "mem/pmem.h"
#include "mem/vmem.h"
#include "proc/cpu.h"
#include "proc/initcode.h"
#include "common.h"
#include "memlayout.h"
#include "riscv.h"
#include "fs/fs.h"

/*----------------外部空间------------------*/

// in trampoline.S
extern char trampoline[];

// in swtch.S
extern void swtch(context_t* old, context_t* new);

// in trap_user.c
extern void trap_user_return();

/*----------------本地变量------------------*/

// 进程数组
static proc_t procs[NPROC];

// 第一个进程
static proc_t* proczero;

// 全局的pid和保护它的锁 
static int global_pid = 1;
static spinlock_t lk_pid;

// wait_lock 用于保护父子进程关系和 wait/exit 的原子性
static spinlock_t wait_lock;

// 申请一个pid(锁保护)
static int alloc_pid()
{
    int tmp = 0;
    spinlock_acquire(&lk_pid);
    assert(global_pid >= 0, "alloc_pid: overflow");
    tmp = global_pid++;
    spinlock_release(&lk_pid);
    return tmp;
}

// 释放锁 + 调用 trap_user_return
static void fork_return()
{
    static bool first = true;
    // 由于调度器中上了锁，所以这里需要解锁
    proc_t* p = myproc();
    spinlock_release(&p->lk);

    if (first) {
        // File system initialization must be run in the context of a
        // regular process (e.g., because it calls sleep), and thus cannot
        // be run from main().
        first = false;
        fs_init();
        
        // ensure other cores see first=0.
        __sync_synchronize();
    }

    trap_user_return();
}

// 返回一个未使用的进程空间
// 设置pid + 设置上下文中的ra和sp
// 申请tf和pgtbl使用的物理页
proc_t* proc_alloc()
{
    proc_t *p;

    for(p = procs; p < &procs[NPROC]; p++) {
        spinlock_acquire(&p->lk);
        if(p->state == UNUSED) {
            goto found;
        } else {
            spinlock_release(&p->lk);
        }
    }
    return NULL;

found:
    p->pid = alloc_pid();
    p->state = USED; // Mark as used, but not runnable yet

    // Allocate trapframe
    if((p->tf = (trapframe_t*)pmem_alloc(true)) == NULL){
        proc_free(p);
        spinlock_release(&p->lk);
        return NULL;
    }
    memset(p->tf, 0, PGSIZE);

    // Initialize page table
    if((p->pgtbl = proc_pgtbl_init((uint64)p->tf)) == NULL){
        proc_free(p);
        spinlock_release(&p->lk);
        return NULL;
    }

    // 如果是 proc_make_first 调用 proc_alloc，它会随后手动分配初始的 mmap 区域
    //（MMAP_BEGIN 到 MMAP_END）。
    // 如果是 proc_fork 调用 proc_alloc，它会随后从父进程复制 mmap 链表。
    // 在这里置空即可
    p->mmap = NULL;

    // Set up context to return to fork_return
    memset(&p->ctx, 0, sizeof(p->ctx));
    p->ctx.ra = (uint64)fork_return;
    p->ctx.sp = p->kstack + PGSIZE;

    return p;
}

// 释放一个进程空间
// 释放pgtbl的整个地址空间
// 释放mmap_region到仓库
// 设置其余各个字段为合适初始值
// tips: 调用者需持有p->lk
void proc_free(proc_t* p)
{
    if(p->tf)
        pmem_free((uint64)p->tf, true);
    p->tf = 0;

    if(p->pgtbl)
        uvm_destroy_pgtbl(p->pgtbl);
    p->pgtbl = 0;

    while(p->mmap){
        mmap_region_t* next = p->mmap->next;
        mmap_region_free(p->mmap);
        p->mmap = next;
    }
    p->mmap = 0;

    p->pid = 0;
    p->parent = 0;
    p->heap_top = 0;
    p->ustack_pages = 0;
    p->exit_state = 0;
    p->state = UNUSED;
}

// 进程模块初始化
void proc_init()
{
    spinlock_init(&lk_pid, "pid_lock");
    spinlock_init(&wait_lock, "wait_lock");
    for(proc_t *p = procs; p < &procs[NPROC]; p++) {
        spinlock_init(&p->lk, "proc_lock");
        p->kstack = KSTACK((int)(p - procs));
        // No need to call proc_free as it expects lock held and handles allocated resources
        p->state = UNUSED;
    }
}
// 获得一个初始化过的用户页表
// 完成了trapframe 和 trampoline 的映射
pgtbl_t proc_pgtbl_init(uint64 trapframe)
{
    pgtbl_t pgtbl = (pgtbl_t)pmem_alloc(true);
    if(pgtbl == NULL) return NULL;
    memset(pgtbl, 0, PGSIZE);

    // 映射 trampoline
    vm_mappages(pgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X); 

    // 映射 trapframe
    vm_mappages(pgtbl, TRAPFRAME, trapframe, PGSIZE, PTE_R | PTE_W);

    return pgtbl;
}

/*
    第一个用户态进程的创建
    它的代码和数据位于initcode.h的initcode数组

    第一个进程的用户地址空间布局:
    trapoline   (1 page)
    trapframe   (1 page)
    ustack      (1 page)
    .......
                        <--heap_top
    code + data (1 page)
    empty space (1 page) 最低的4096字节 不分配物理页，同时不可访问
*/
void proc_make_first()
{
    proc_t *p;

    p = proc_alloc();
    if(p == NULL) panic("proc_make_first: alloc failed");
    proczero = p;

    // ustack 映射 + 设置 ustack_pages 
    uint64 ustack_va = TRAPFRAME - PGSIZE;
    void *ustack_pa = pmem_alloc(true);
    if(ustack_pa == NULL) panic("proc_make_first: alloc ustack failed");
    vm_mappages(p->pgtbl, ustack_va, (uint64)ustack_pa, PGSIZE, PTE_R | PTE_W | PTE_U);
    p->ustack_pages = 1;

    // initcode 映射
    uint64 initcode_va = CODE_TEXT_START;
    void *initcode_pa = pmem_alloc(true);
    if(initcode_pa == NULL) panic("proc_make_first: alloc initcode failed");
    
    // data + code 映射
    assert(initcode_len <= PGSIZE, "proc_make_first: initcode too big\n");
    
    memmove(initcode_pa, initcode, sizeof(initcode));
    asm volatile("fence.i"); // 查看 doc/fence.i.md
    vm_mappages(p->pgtbl, initcode_va, (uint64)initcode_pa, PGSIZE, PTE_R | PTE_W | PTE_X | PTE_U);
    
    // 设置 heap_top    
    p->heap_top = initcode_va + PGSIZE;

    // 设置 mmap_region_t
    p->mmap = mmap_region_alloc();
    if(p->mmap == NULL) panic("proc_make_first: alloc mmap failed");
    p->mmap->begin = MMAP_BEGIN;
    p->mmap->npages = (MMAP_END - MMAP_BEGIN) / PGSIZE;
    p->mmap->next = NULL;

    // 设置 trapframe
    p->tf->epc = initcode_va;
    p->tf->sp = ustack_va + PGSIZE;

    p->state = RUNNABLE;
    spinlock_release(&p->lk);
}

// 进程复制
// UNUSED -> RUNNABLE
int proc_fork()
{
    int pid;
    proc_t *np;
    proc_t *p = myproc();

    // Allocate process
    if((np = proc_alloc()) == NULL){
        return -1;
    }

    // Copy user memory from parent to child
    uvm_copy_pgtbl(p->pgtbl, np->pgtbl, p->heap_top, p->ustack_pages, p->mmap);
    np->heap_top = p->heap_top;
    np->ustack_pages = p->ustack_pages;

    // Copy saved user registers
    *(np->tf) = *(p->tf);

    // Cause fork to return 0 in the child
    np->tf->a0 = 0;

    // Copy mmap list
    mmap_region_t *node = p->mmap; // 指向父进程的空闲链表头
    mmap_region_t **prev = &np->mmap; // 指向子进程链表头的指针地址（用于构建链表）
    while(node){
        mmap_region_t *new_node = mmap_region_alloc(); // 为子进程分配一个新的节点结构体
        if(new_node == NULL){
            proc_free(np);
            spinlock_release(&np->lk);
            return -1;
        }
        *new_node = *node; // 复制节点内容（起始地址、页数等）

        new_node->next = NULL; 
        *prev = new_node; // 将新节点挂到子进程链表的末尾
        prev = &new_node->next; // 更新 prev 指向新节点的 next 指针地址
        node = node->next; // 继续处理父进程的下一个节点
    }

    // increment reference counts on open file descriptors
    // (Not implemented yet, but would go here)

    pid = np->pid;

    spinlock_release(&np->lk);

    // Set parent under wait_lock to ensure atomicity with wait/exit
    spinlock_acquire(&wait_lock);
    np->parent = p;
    spinlock_release(&wait_lock);

    spinlock_acquire(&np->lk);
    np->state = RUNNABLE;
    spinlock_release(&np->lk);

    return pid;
}

// 进程放弃CPU的控制权
// RUNNING -> RUNNABLE
void proc_yield()
{
    proc_t *p = myproc();
    spinlock_acquire(&p->lk);
    p->state = RUNNABLE;
    proc_sched();
    spinlock_release(&p->lk);
}

// 等待一个子进程进入 ZOMBIE 状态
// 将退出的子进程的exit_state放入用户给的地址 addr
// 成功返回子进程pid，失败返回-1
int proc_wait(uint64 addr)
{
    int havekids, pid;
    proc_t *p = myproc();

    spinlock_acquire(&wait_lock);

    for(;;){
        // Scan through table looking for exited children
        havekids = 0;
        for(proc_t *np = procs; np < &procs[NPROC]; np++){
            if(np->parent == p){
                spinlock_acquire(&np->lk);
                havekids = 1;
                if(np->state == ZOMBIE){
                    // Found one
                    pid = np->pid;
                    if(addr != 0){
                        uvm_copyout(p->pgtbl, addr, (uint64)&np->exit_state, sizeof(np->exit_state));
                    }
                    proc_free(np);
                    spinlock_release(&np->lk);
                    spinlock_release(&wait_lock);
                    return pid;
                }
                spinlock_release(&np->lk);
            }
        }

        // No point waiting if we don't have any children
        if(!havekids || p->state == ZOMBIE){ // killed check omitted for now
            spinlock_release(&wait_lock);
            return -1;
        }

        // Wait for children to exit.
        // sleep releases wait_lock and re-acquires it on wakeup.
        proc_sleep(p, &wait_lock);
    }
}

// 父进程退出，子进程认proczero做父，因为它永不退出
// Caller must hold wait_lock.
static void proc_reparent(proc_t* parent)
{
    for(proc_t *p = procs; p < &procs[NPROC]; p++){
        if(p->parent == parent){
            p->parent = proczero;
            proc_wakeup(proczero);
        }
    }
}

// 唤醒一个进程(需要持有锁)
// static void proc_wakeup_one(proc_t* p)
// {
//     assert(spinlock_holding(&p->lk), "proc_wakeup_one: lock");
//     if(p->state == SLEEPING) { // Ignore sleep_space check for now as per instructions? Or just basic check.
//         p->state = RUNNABLE;
//     }
// }

// 进程退出
void proc_exit(int exit_state)
{
    proc_t *p = myproc();

    if(p == proczero)
        panic("init exiting");

    // Close all open files (not implemented)

    spinlock_acquire(&wait_lock);

    // Give any children to init
    proc_reparent(p);

    // Parent might be sleeping in wait()
    proc_wakeup(p->parent);

    spinlock_acquire(&p->lk);

    p->exit_state = exit_state;
    p->state = ZOMBIE;

    spinlock_release(&wait_lock);

    // Jump into the scheduler, never to return.
    proc_sched();
    panic("zombie exit");
}

// 进程切换到调度器
// ps: 调用者保证持有当前进程的锁
void proc_sched()
{
    int intena;
    proc_t *p = myproc();

    if(!spinlock_holding(&p->lk))
        panic("sched p->lk");
    if(mycpu()->noff != 1)
        panic("sched locks");
    if(p->state == RUNNING)
        panic("sched running");
    if(intr_get())
        panic("sched interruptible");

    intena = mycpu()->origin;
    swtch(&p->ctx, &mycpu()->ctx);
    mycpu()->origin = intena;
}

// 调度器
void proc_scheduler()
{
    proc_t *p;
    struct cpu *c = mycpu();
    
    c->proc = 0;
    for(;;){
        // Avoid deadlock by ensuring devices can interrupt
        intr_on();
        intr_off();

        int found = 0;
        for(p = procs; p < &procs[NPROC]; p++) {
            spinlock_acquire(&p->lk);
            if(p->state == RUNNABLE) {
                // Switch to chosen process
                p->state = RUNNING;
                c->proc = p;
                // printf("cpu %d run proc %d\n", mycpuid(), p->pid);
                swtch(&c->ctx, &p->ctx);

                // Process is done running for now
                c->proc = 0;
                found = 1;
            }
            spinlock_release(&p->lk);
        }
        
        if(found == 0) { // 注意这里在循环内
            // nothing to run; stop running on this core until an interrupt.
            // Wait For Interrupt：这是一条特殊的 RISC-V 指令。它告诉 CPU 进入低功耗模式，暂停执行，直到下一个中断信号到来（不需要中断enable）。
            asm volatile("wfi");
        }
    }
}

// 进程睡眠在sleep_space
// Atomically release lk and sleep on sleep_space.
// Reacquires lk when awakened.
void proc_sleep(void* sleep_space, spinlock_t* lk)
{
    proc_t *p = myproc();
    
    // Must acquire p->lk in order to
    // change p->state and then call sched.
    // Once we hold p->lk, we can be guaranteed that we won't
    // miss any wakeup (wakeup locks p->lk),
    // so it's okay to release lk.

    spinlock_acquire(&p->lk);
    spinlock_release(lk);

    // Go to sleep.
    p->sleep_space = sleep_space;
    p->state = SLEEPING;

    proc_sched();

    // Tidy up.
    p->sleep_space = 0;

    // Reacquire original lock.
    spinlock_release(&p->lk);
    spinlock_acquire(lk);
}

// 唤醒所有在sleep_space沉睡的进程
void proc_wakeup(void* sleep_space)
{
    proc_t *p;

    for(p = procs; p < &procs[NPROC]; p++) {
        if(p != myproc()){
            spinlock_acquire(&p->lk);
            if(p->state == SLEEPING && p->sleep_space == sleep_space) {
                p->state = RUNNABLE;
            }
            spinlock_release(&p->lk);
        }
    }
}