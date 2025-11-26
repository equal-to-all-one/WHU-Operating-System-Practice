#include "riscv.h"
#include "lib/print.h"
#include "lib/str.h"
#include "mem/pmem.h"
#include "mem/vmem.h"
#include "trap/trap.h"
#include "dev/timer.h"
#include "dev/uart.h"
volatile static int started = 0;

int main()
{
    int cpuid = r_tp();

    if(cpuid == 0) {
        
        print_init();
        printf("cpu %d is booting!\n", cpuid);
        uart_init();
        pmem_init();
        kvm_init();
        kvm_inithart();
        trap_kernel_init();
        trap_kernel_inithart();
        timer_create();
        intr_on();

        __sync_synchronize();
        started = 1;

    } else {

        while(started == 0);
        
        __sync_synchronize();
        printf("cpu %d is booting!\n", cpuid);

        kvm_inithart();
        trap_kernel_inithart();
        intr_on();
    }
    while (1);    
}

