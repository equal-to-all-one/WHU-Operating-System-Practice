#include "lib/print.h"
#include "proc/cpu.h"
#include "mem/mmap.h"
#include "mem/vmem.h"
#include "syscall/syscall.h"
#include "syscall/sysnum.h"
#include "syscall/sysfunc.h"

// 系统调用跳转
static uint64 (*syscalls[])(void) = {
    [SYS_exec]          sys_exec,
    [SYS_brk]           sys_brk,
    [SYS_mmap]          sys_mmap,
    [SYS_munmap]        sys_munmap,
    [SYS_fork]          sys_fork,
    [SYS_wait]          sys_wait,
    [SYS_exit]          sys_exit,
    [SYS_sleep]         sys_sleep,
    [SYS_open]          sys_open,
    [SYS_close]         sys_close,
    [SYS_read]          sys_read,
    [SYS_write]         sys_write,
    [SYS_lseek]         sys_lseek,
    [SYS_dup]           sys_dup,
    [SYS_fstat]         sys_fstat,
    [SYS_getdir]        sys_getdir,
    [SYS_mkdir]         sys_mkdir,
    [SYS_chdir]         sys_chdir,
    [SYS_link]          sys_link,
    [SYS_unlink]        sys_unlink,

};

// 系统调用
void syscall()
{
    proc_t* p = myproc();
    int num = p->tf->a7;

    if(num >= 0 && num < sizeof(syscalls)/sizeof(syscalls[0]) && syscalls[num]) {
        /*  xv6: p->tf->a0 = syscalls[num]();
            注意这里的重新加载，这一点与xv6不同，我们的proc_exec重新分配了trapframe，
            所以我们需要将结果写入新的trapframe中，所以我们需要重新获取当前进程，为了
            重新获取新的trapframe。
            在xv6原本的实现中，kexec没有重新分配trapframe，而是复用了原来的trapframe，
            所以不需要重新获取当前进程。
            但是如果我们仍然使用上面的源代码的话，就会出现一个问题：
            p->tf->a0 可能被缓存在寄存器中，这是原来的trapframe，
            我们将结果写入了这个trapframe中，这就导致我们新的trapframe中的a0没有被正确设置，
            从而导致用户态无法获取正确的返回值。
        */
        uint64 ret = syscalls[num]();
        p = myproc(); // reload p->tf in case of exec
        p->tf->a0 = ret;
    } else {
        printf("pid %d %s: unknown sys call %d\n", p->pid, "syscall", num);
        p->tf->a0 = -1;
    }
}

/*
    其他用于读取传入参数的函数
    参数分为两种,第一种是数据本身,第二种是指针
    第一种使用tf->ax传递
    第二种使用uvm_copyin 和 uvm_copyinstr 进行传递
*/

// 读取 n 号参数,它放在 an 寄存器中
static uint64 arg_raw(int n)
{   
    proc_t* proc = myproc();
    switch(n) {
        case 0:
            return proc->tf->a0;
        case 1:
            return proc->tf->a1;
        case 2:
            return proc->tf->a2;
        case 3:
            return proc->tf->a3;
        case 4:
            return proc->tf->a4;
        case 5:        
            return proc->tf->a5;
        default:
            panic("arg_raw: illegal arg num");
            return -1;
    }
}

// 读取 n 号参数, 作为 uint32 存储
void arg_uint32(int n, uint32* ip)
{
    *ip = arg_raw(n);
}

// 读取 n 号参数, 作为 uint64 存储
void arg_uint64(int n, uint64* ip)
{
    *ip = arg_raw(n);
}

// 读取 n 号参数指向的字符串到 buf, 字符串最大长度是 maxlen
void arg_str(int n, char* buf, int maxlen)
{
    proc_t* p = myproc();
    uint64 addr;
    arg_uint64(n, &addr);

    uvm_copyin_str(p->pgtbl, (uint64)buf, addr, maxlen);
}