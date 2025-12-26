#include "lib/print.h"
#include "lib/str.h"
#include "mem/pmem.h"
#include "mem/vmem.h"
#include "proc/cpu.h"
#include "proc/initcode.h"
#include "memlayout.h"

// in trampoline.S
extern char trampoline[];

// in swtch.S
extern void swtch(context_t* old, context_t* new);

// in trap_user.c
extern void trap_user_return();


// 第一个进程
static proc_t proczero;

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
    // pid 设置
    proczero.pid = 1;

    // 分配 trapframe
    proczero.tf = (trapframe_t*)pmem_alloc(true);
    if(proczero.tf == NULL) panic("proc_make_first: alloc tf failed");
    memset(proczero.tf, 0, PGSIZE);

    // pagetable 初始化
    proczero.pgtbl = proc_pgtbl_init((uint64)proczero.tf);
    if(proczero.pgtbl == NULL) panic("proc_make_first: init pgtbl failed");

    // ustack 映射 + 设置 ustack_pages 
    uint64 ustack_va = TRAPFRAME - PGSIZE;
    void *ustack_pa = pmem_alloc(true);
    if(ustack_pa == NULL) panic("proc_make_first: alloc ustack failed");
    vm_mappages(proczero.pgtbl, ustack_va, (uint64)ustack_pa, PGSIZE, PTE_R | PTE_W | PTE_U);
    proczero.ustack_pages = 1;

    // initcode 映射
    uint64 initcode_va = 0x1000;
    void *initcode_pa = pmem_alloc(true);
    if(initcode_pa == NULL) panic("proc_make_first: alloc initcode failed");
    
    // data + code 映射
    assert(initcode_len <= PGSIZE, "proc_make_first: initcode too big\n");
    
    memmove(initcode_pa, initcode, sizeof(initcode));
    asm volatile("fence.i");
    vm_mappages(proczero.pgtbl, initcode_va, (uint64)initcode_pa, PGSIZE, PTE_R | PTE_W | PTE_X | PTE_U);
    
    // 设置 heap_top    
    proczero.heap_top = initcode_va + PGSIZE;

    // 设置 kstack
    proczero.kstack = KSTACK(0);

    // 设置 context
    proczero.ctx.ra = (uint64)trap_user_return;
    proczero.ctx.sp = proczero.kstack + PGSIZE;

    // 设置 trapframe
    proczero.tf->epc = initcode_va;
    proczero.tf->sp = ustack_va + PGSIZE;

    // 切换进程
    mycpu()->proc = &proczero;
    swtch(&mycpu()->ctx, &proczero.ctx);
}