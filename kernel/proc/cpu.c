#include "proc/cpu.h"
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
struct cpu* mycpu(void)
{
    int id = mycpuid();
    struct cpu *c = &cpus[id];
    return c;
}