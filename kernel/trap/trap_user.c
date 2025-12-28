#include "lib/print.h"
#include "trap/trap.h"
#include "proc/cpu.h"
#include "mem/vmem.h"
#include "memlayout.h"
#include "riscv.h"
#include "syscall/syscall.h"

// in trampoline.S
extern char trampoline[];      // 内核和用户切换的代码
extern char user_vector[];     // 用户触发trap进入内核
extern char user_return[];     // trap处理完毕返回用户

// in trap.S
extern char kernel_vector[];   // 内核态trap处理流程

// in trap_kernel.c
extern char* interrupt_info[16]; // 中断错误信息
extern char* exception_info[16]; // 异常错误信息
extern void timer_interrupt_handler();
extern void external_interrupt_handler();

// 在user_vector()里面调用
// 用户态trap处理的核心逻辑
void trap_user_handler()
{
    uint64 sepc = r_sepc();          // 记录了发生异常时的pc值
    uint64 sstatus = r_sstatus();    // 与特权模式和中断相关的状态信息
    uint64 scause = r_scause();      // 引发trap的原因
    uint64 stval = r_stval();        // 发生trap时保存的附加信息(不同trap不一样)
    proc_t* p = myproc();

    // 确认trap来自U-mode
    assert((sstatus & SSTATUS_SPP) == 0, "trap_user_handler: not from u-mode");

    // 设置内核trap vector，因为我们在内核态了
    w_stvec((uint64)kernel_vector);

    // 保存 epc 到 trapframe
    p->tf->epc = sepc;

    int trap_id = scause & 0xf; // 低4位是标识符

    if (scause & (1L << 63)) {
        // Interrupt
        switch (trap_id) {
            case 1: // Supervisor software interrupt (Timer)
                timer_interrupt_handler();
                proc_yield();
                break;
            case 9: // Supervisor external interrupt (PLIC)
                external_interrupt_handler();
                break;
            default:
                printf("unknown user interrupt: %d\n", trap_id);
                break;
        }
    } else {
        // Exception
        if (trap_id == 8) { // Environment call from U-mode
            // printf("get a syscall from proc %d\n", p->pid);
            p->tf->epc += 4; // Skip ecall instruction
            intr_on();
            syscall();
        } else {
            printf("user exception: %s\n", exception_info[trap_id]);
            printf("sepc=%p stval=%p\n", sepc, stval);
            panic("trap_user_handler: unhandled exception");
        }
    }

    trap_user_return();
}

// 调用user_return()
// 内核态返回用户态
void trap_user_return()
{
    proc_t* p = myproc();

    // 关闭中断
    intr_off();

    // 设置 stvec 指向 user_vector (trampoline)
    uint64 trampoline_uservec = TRAMPOLINE + (user_vector - trampoline);
    w_stvec(trampoline_uservec);

    // 准备 trapframe
    p->tf->kernel_satp = r_satp(); // kernel page table
    p->tf->kernel_sp = p->kstack + PGSIZE; // kernel stack top
    p->tf->kernel_trap = (uint64)trap_user_handler;
    p->tf->kernel_hartid = r_tp(); // hartid

    // 设置 sstatus
    // SSTATUS_SPP = 0 (User mode)
    // SSTATUS_SPIE = 1 (Enable interrupts in user mode)
    unsigned long x = r_sstatus();
    x &= ~SSTATUS_SPP; // clear SPP to 0 for user mode
    x |= SSTATUS_SPIE; // enable interrupts in user mode
    w_sstatus(x);

    // 设置 sepc
    w_sepc(p->tf->epc);

    // 生成用户页表 satp
    uint64 satp = MAKE_SATP(p->pgtbl);

    // 跳转到 trampoline 的 user_return
    uint64 trampoline_userret = TRAMPOLINE + (user_return - trampoline);
    // 注意这里不要使用proc的tf，因为trampoline中user_return先换回了用户页表
    // 而tf是保存在内核下的直接映射虚拟地址，TRAPFRAME在用户页表中有其特殊的映射
    ((void (*)(uint64, uint64))trampoline_userret)(TRAPFRAME, satp);
}