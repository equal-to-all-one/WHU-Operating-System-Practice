#include "proc/cpu.h"
#include "lib/lock.h"
#include "riscv.h"

static cpu_t cpus[NCPU];

// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
int mycpuid(void) 
{
    // 在start.c 中， 将每个 CPU 的 hartid 存储在其 tp 寄存器中
    int id = r_tp();
    return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
cpu_t* mycpu(void)
{
    int id = mycpuid();
    cpu_t *c = &cpus[id];
    return c;
}

proc_t* myproc(void)
{
    push_off(); // disable interrupts to avoid being rescheduled to another CPU
    struct cpu *c = mycpu();
    struct proc *p = c->proc;
    pop_off();
    return p;
}