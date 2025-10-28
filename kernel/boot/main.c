#include "riscv.h"
#include "lib/print.h"
#include "proc/cpu.h"

volatile static int started = 0;

int main()
{
    if(mycpuid() == 0){
        print_init();
        printf("\n");
        printf("cpu0 is booting\n");
        __sync_synchronize();
        started = 1;
    } else{
        while(started == 0)
            ;
        __sync_synchronize(); // 内存屏障，确保指令不会被重排序，从而正确看到 started 变量的更新
        printf("cpu1 is booting\n");
    }

    while(1)
        ;
}