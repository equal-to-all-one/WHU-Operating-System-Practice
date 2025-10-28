#include "riscv.h"

__attribute__ ((aligned (16))) uint8 CPU_stack[4096 * NCPU];

int main();

void start()
{
    // set the M Previous Privilege mode to Supervisor, for mret.
    unsigned long  mstatus = r_mstatus();
    mstatus &= ~MSTATUS_MPP_MASK; // clear MPP bits
    mstatus |= MSTATUS_MPP_S; // set MPP to Supervisor mode(01)
    w_mstatus(mstatus); // write the modified mstatus back to the mstatus CSR

    // set the M Exception Program Counter to main(), for mret.
    w_mepc((uint64)main);

    // disable paging for now.
    w_satp(0);
        
    // delegate all interrupts and exceptions to supervisor mode.
    w_medeleg(0xffff);
    w_mideleg(0xffff);

    // 允许机器模式下的软件中断和外部中断（SEIE)、时钟中断(STIE)
    w_sie(r_sie() | SIE_SEIE | SIE_STIE);

    // configure Physical Memory Protection to give supervisor mode
    // access to all of physical memory.
    // U模式也被允许访问全部物理内存，但是U模式下的地址访问会被更强大的虚拟内存机制来控制
    w_pmpaddr0(0x3fffffffffffffull); // 56位地址的55-2位， SV39架构下，物理地址为56位(指导书3.1节)
    w_pmpcfg0(0xf);

    // keep each CPU's hartid in its tp register, for cpuid().
    int id = r_mhartid();
    w_tp(id);

    // TODO: timerinit()

    // switch to supervisor mode and jump to main().
    asm volatile("mret");

}