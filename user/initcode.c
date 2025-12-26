#include "sys.h"

void start()
{
    syscall(SYS_print);
    syscall(SYS_print);
    while(1);
}