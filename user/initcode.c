#include "sys.h"

// 与内核保持一致
#define VA_MAX       (1ul << 38)
#define PGSIZE       4096
#define MMAP_END     (VA_MAX - 34 * PGSIZE)
#define MMAP_BEGIN   (MMAP_END - 8096 * PGSIZE) 

char *str1, *str2;

int start()
{
    int pid = syscall(SYS_fork);

    if (pid == 0) {
        syscall(SYS_print, "Child (pid 2): sleeping for 2 seconds...\n");
        syscall(SYS_sleep, 2);
        syscall(SYS_print, "Child (pid 2): woke up!\n");
        syscall(SYS_exit, 0);
    } else {
        syscall(SYS_print, "Parent (pid 1): sleeping for 4 seconds...\n");
        syscall(SYS_sleep, 4);
        syscall(SYS_print, "Parent (pid 1): woke up!\n");
        int exit_state;
        syscall(SYS_wait, &exit_state);
        syscall(SYS_print, "Parent: child exited, test done.\n");
    }

    while(1);
    return 0;
}