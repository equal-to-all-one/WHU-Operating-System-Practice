LAB-6: 进程管理模块
代码组织结构
ECNU-OSLAB
├── include
│ ├── dev
│ │ ├── timer.h
│ │ ├── plic.h
│ │ └── uart.h
│ ├── lib
│ │ ├── print.h
│ │ ├── lock.h (CHANGE) 新增睡眠锁
│ │ └── str.h
│ ├── proc
│ │ ├── proc.h (CHANGE) 完善进程管理
│ │ ├── initcode.h
│ │ └── cpu.h
│ ├── mem
│ │ ├── mmap.h
│ │ ├── pmem.h
│ │ └── vmem.h
│ ├── trap
│ │ └── trap.h
│ ├── syscall
│ │ ├── syscall.h
│ │ ├── sysfunc.h (CHANGE) 新的系统调用
│ │ └── sysnum.h (CHANGE) 新的系统调用
│ ├── common.h (CHANGE) 增加NPROC定义
│ ├── memlayout.h
│ └── riscv.h
├── kernel
│ ├── boot
│ │ ├── main.c (CHANGE) 日常更新
│ │ ├── start.c
│ │ ├── entry.S
│ │ └── Makefile
│ ├── dev
│ │ ├── uart.c
│ │ ├── timer.c (CHANGE) 系统时钟取消static
│ │ ├── plic.c
│ │ └── Makefile
│ ├── lib
│ │ ├── print.c
│ │ ├── spinlock.c
│ │ ├── sleeplock.c (TODO)
│ │ ├── str.c
│ │ └── Makefile
│ ├── proc
│ │ ├── cpu.c
│ │ ├── proc.c (CHANGE) 这次实验的核心
│ │ ├── swtch.S
│ │ └── Makefile
│ ├── mem
│ │ ├── pmem.c
│ │ ├── kvm.c
│ │ ├── uvm.c
│ │ ├── mmap.c
│ │ └── Makefile
│ ├── syscall
│ │ ├── syscall.c (CHANGE) 日常更新
│ │ ├── sysfunc.c (CHANGE) 日常更新
│ │ └── Makefile
│ ├── trap
│ │ ├── trap_kernel.c (CHANGE) 小修改
│ │ ├── trap_user.c (CHANGE) 小修改
│ │ ├── trap.S
│ │ ├── trampoline.S
│ │ └── Makefile
│ ├── Makefile
│ └── kernel.ld
├── user
│ ├── syscall_arch.h
│ ├── syscall_num.h (CHANGE) 日常更新
│ ├── sys.h
│ ├── initcode.c (CHANGE) 日常更新
│ └── Makefile
├── Makefile
└── common.mk

标记说明

TODO 本次新增的文件 + 其中有函数待实现 (核心部分)

NEW 本次新增的文件 + 无需做修改 (辅助部分)

CHANGE 本来就有的文件 + 需要做修改 或 助教做了修改 (整体兼容)

准备阶段
系统调用的修改:这部分助教已经做好了

首先删去上一次的临时系统调用: sys_copyin() sys_copyout() sys_copyinstr()

添加新的系统调用 sys_print() sys_fork() sys_exit() sys_wait() sys_sleep() 其中第一个也是临时的

这次我们将完成进程管理模块, 主要是做两件事:

进程调度:支持多进程轮流使用CPU(从单进程到多进程)

进程生命周期:UNUSED RUNNABLE RUNNING SLEEPING ZOMBIE 五个状态的转换

任务一: fork + exit + wait
首先我们定义进程数组, 单进程时代的 proczero 变成指针而不再是具体的进程

// 进程数组
static proc_t procs[NPROC];

// 第一个进程的指针
static proc_t* proczero;

// 全局的pid和保护它的锁 
static int global_pid = 1;
static spinlock_t lk_pid;
由于要引入多个进程, 进程的定义在之前的基础上做了扩充

typedef struct proc {
    
    spinlock_t lk;           // 自旋锁

    /* 下面的五个字段需要持有锁才能修改 */

    int pid;                 // 标识符
    enum proc_state state;   // 进程状态
    struct proc* parent;     // 父进程
    int exit_state;          // 进程退出时的状态(父进程可能关心)
    void* sleep_space;       // 睡眠是在等待什么

    pgtbl_t pgtbl;           // 用户态页表
    uint64 heap_top;         // 用户堆顶(以字节为单位)
    uint64 ustack_pages;     // 用户栈占用的页面数量
    mmap_region_t* mmap;     // 用户可映射区域的起始节点
    trapframe_t* tf;         // 用户态内核态切换时的运行环境暂存空间

    uint64 kstack;           // 内核栈的虚拟地址
    context_t ctx;           // 内核态进程上下文
} proc_t;
考虑到 xv6 中 killed 字段处理的复杂性, 我们暂时不声明和使用这个字段

在任务一中, 我们忽略 sleep_space 和进程的 SLEEPING 状态

考虑一个问题:为什么需要自旋锁保护这五个字段？

看起来任意进程在申请后应该是私有的, 不会出现并发错误

实际上进程也可能关心其他进程的某些字段, 你可以在xv6里面搜索 proc[NPROC], 可以找到很多遍历操作

这种遍历可能带来的错误是:进程A读取进程B的字段, 同时进程B修改这个字段, 构成读者写者问题

于是, 我们在读和写这些字段的时候要先上锁(有例外情况, 出现例外的地方xv6给出了注释)

在后面的函数实现过程中, 请多思考这个锁字段的使用

首先实现以下三个函数:

proc_init() 初始化一些全局变量, 设置kstack字段并调用 proc_free()

proc_alloc() 从进程数组里申请一个进程, 并申请资源和初始化一些字段

proc_free() 向进程数组里归还一个进程, 并释放资源和清空一些字段

这三个函数是进程数组这一全局资源的控制函数, 因此将他们视为一组操作

然后需要考虑修改 proc_make_first()

由于可以直接调用 proc_alloc(), 一些操作可以删去

由于后面要引入调度器, proc_make_first() 无需调用 swtch(), 直接返回即可

接着考虑如何从单进程到多进程, 难道每个进程都像 proczero 这样从零开始吗？

更好的做法是使用 proc_fork() 函数进行复制, 主要是复制地址空间的五个字段

这样产生的新进程被称为子进程, 而被它复制的进程称为父进程, 用 parent 字段标记

于是, 可以发现各个进程会组成一个由 proczero 作为根节点的进程树

这两个进程在用户态怎么区分呢？解决方案是让他们的返回值不同

对于父进程, 它收到的返回值是子进程的pid, 通过函数返回做到

对于子进程, 它收到的是0, 通过修改 p->tf->a0 做到

完成 proc_fork() 后我们进入下一组函数:

proc_wait() 等待子进程退出，调用 proc_free() 回收子进程

proc_exit() 进程退出

这三个函数构成一个常见的组合:

int pid = fork(); // 分支

if(pid == 0) { // 子进程
    do something ...
    exit(0);
} else { // 父进程
    int exit_state;
    wait(&exit_state);
    do something ....
}
这两个函数的实现过程会遇到两个问题:

由于进程的树形结构，如果出现父进程退出但子进程没退出的情况该怎么办呢？

在 proc_exit() 函数里调用 proc_reparent() 将当前进程的孩子托付给 proczero

因为它是整棵进程树的根，是不会退出的

由于 proc_wait() 函数是一个循环, 所以没等到子进程退出就会一直占用CPU

所以添加一个新的函数 proc_yield(), 进程放弃CPU使用权进入调度

这里就会引出调度函数 proc_sched() 和 proc_scheduler()

这两个函数是调度的两个阶段:

第一阶段:当前CPU的用户进程 -> CPU自己的进程(之前称之为高级进程)

第二阶段:CPU自己的坚持 -> 被选中的新用户进程

当占用CPU的进程是CPU自己的进程时, mycpu()->proc = NULL

两个阶段的核心操作都是 swtch() 做上下文切换

proc_sched() 需要做一些检查以确保切换的安全

proc_scheduler() 需要挑选新的RUNNABLE用户进程, 这里采用各进程按顺序轮流执行的调度算法

当调度器启动后, CPU自己的进程就被困在调度器里了(死循环), 它的上下文就是调度器的运行环境

在 main() 的最后, 各个CPU的进程都会进入调度器, 并忠实地留在这里

于是进程切换的过程:proc-1 -> CPU进程(调度器)-> proc-2

于是我们可以总结出CPU自己的进程做的事:初始化OS各个模块, 然后作为用户进程调度器

之后各个CPU的调度器会从进程数组里选择RUNNABLE的用户进程执行

测试一
当完成上述函数后, 请建立四个新的系统调用:

sys_print 接受一个用户的字符串地址并输出

sys_fork

sys_wait

sys_exit 接受一个用户的exit_state存储地址

之后使用单个CPU执行这段 initcode.c 里的代码 (理解它在测试什么)

#include "sys.h"

// 与内核保持一致
#define VA_MAX       (1ul << 38)
#define PGSIZE       4096
#define MMAP_END     (VA_MAX - 34 * PGSIZE)
#define MMAP_BEGIN   (MMAP_END - 8096 * PGSIZE) 

char *str1, *str2;

int main()
{
    syscall(SYS_print, "\nuser begin\n");

    // 测试MMAP区域
    str1 = (char*)syscall(SYS_mmap, MMAP_BEGIN, PGSIZE);
    
    // 测试HEAP区域
    long long top = syscall(SYS_brk, 0);
    str2 = (char*)top;
    syscall(SYS_brk, top + PGSIZE);

    str1[0] = 'M';
    str1[1] = 'M';
    str1[2] = 'A';
    str1[3] = 'P';
    str1[4] = '\n';
    str1[5] = '\0';

    str2[0] = 'H';
    str2[1] = 'E';
    str2[2] = 'A';
    str2[3] = 'P';
    str2[4] = '\n';
    str2[5] = '\0';

    int pid = syscall(SYS_fork);

    if(pid == 0) { // 子进程
        for(int i = 0; i < 100000000; i++);
        syscall(SYS_print, "child: hello\n");
        syscall(SYS_print, str1);
        syscall(SYS_print, str2);

        syscall(SYS_exit, 1);
        syscall(SYS_print, "child: never back\n");
    } else {       // 父进程
        int exit_state;
        syscall(SYS_wait, &exit_state);
        if(exit_state == 1)
            syscall(SYS_print, "parent: hello\n");
        else
            syscall(SYS_print, "parent: error\n");
    }

    while(1);
    return 0;
}
理想结果:

图片

任务二: sleep + wakeup
首先考虑之前的 proc_wait() proc_exit() 的实现有什么问题

父进程在等待子进程退出时的行为是: 遍历所有进程, 发现子进程没退出则 proc_yield()

yield 之后父进程还是 RUNNABLE 的, 有可能被调度, 但是子进程退出前这样的调度没有意义

我们需要一种类似中断的机制: 当满足某一条件时执行父进程的代码

于是我们引入睡眠和唤醒这两个函数

将 proc_wait() 中的 proc_yield() 换成 proc_sleep()

在 proc_exit() 中使用 proc_wakeup_one() 主动唤醒父进程

对于 proc_sleep(), 我们可以暂时忽略它传入的锁, 只关心 sleep_space, 这个变量标记了睡眠原因

唤醒函数有两种, proc_wakeup_one() 只被 proc_exit() 调用, 它唤醒指定的单个进程

proc_wakeup() 未来还会在其他地方调用, 它唤醒所有睡在 sleep_space 的进程

举个例子, 如果多个进程竞争一个资源, 那么暂时没有获得这个资源的进程可以进入睡眠, 标记 sleep_space = 这种资源

当使用资源的进程使用完毕后, 调用 proc_wakeup(资源) 唤醒所有等待这种资源的进程

当你完成这这些函数后, proc.c 就完成了, 重新执行任务一的测试以确保睡眠和唤醒的正确性

任务三: 睡眠锁
进程的睡眠和唤醒可以用于设计睡眠锁, 睡眠锁和自旋锁的关系有点像中断和轮询的关系

睡眠锁的核心是设置一个唤醒条件, 自旋锁的核心是不断测试一个条件

typedef struct spinlock {
    int locked;
    char* name;
    int cpuid;
} spinlock_t;

typedef struct sleeplock {
    spinlock_t lk;
    int locked;
    char* name;
    int pid;
} sleeplock_t;
比较这两种锁的定义, 可以发现睡眠锁是在自旋锁的基础上建立的

自旋锁 = 开关中断 + 原子操作 + 轮询

睡眠锁 = 自旋锁 + 睡眠唤醒

睡眠锁中的自旋锁是用于确其它字段被正确并发访问(比如同时修改 locked 字段的操作)

当睡眠锁被占用时, 进程调用 proc_sleep(睡眠锁) 进入睡眠状态, 同时等待睡眠锁

当睡眠锁被释放时, 进程调用 proc_wakeup(睡眠锁) 唤醒等待睡眠锁的进程

值得注意的是: proc_wakeup() 会唤醒所有等待者, 但是只有一个可以拿到资源

其他等待者被唤醒后发现还是没有资源, 会再次睡眠等待, 这叫 "惊群效应"

完成 sleeplock.c 中的四个函数

睡眠锁在后面文件系统中用到, 这里不做测试

任务四: 进程管理与时钟相关的部分
首先考虑一个问题:

如果一个恶意进程一直占用CPU而不调用 proc_yield() 或 proc_sleep()

那么CPU事实上就被它独占了而其他进程只能一直等待

为了避免这样的事情发生, 我们应该提供一种强制进程交出CPU使用权的机制

可以利用时钟中断实现这件事：在发生时钟中断后调用 proc_yield()

注意: 内核和用户的trap处理都涉及时钟, 内核trap处理在调度前要多做一些检查

测试: 在调度器中添加一行输出, 打印被调度的进程编号

执行如下 initcode

#include "sys.h"

int main()
{
    syscall(SYS_fork);
    syscall(SYS_fork);

    while(1);
    return 0;
}
现象如下, 可以发现进程1没有独占CPU而是共享使用

图

考虑为什么产生了四个进程, 如果加一行 syscall(SYS_fork); 会是多少个呢

tips: 在 debug 时, 建议注释掉时钟的 proc_yield(), 并使用单个CPU执行, 这样方便一些

最后, 你还需要添加一个系统调用 sys_sleep() 它接收一个睡眠时间(要求以秒为单位)

与系统时钟配合, 实现进程睡眠一段时间, 这部分由你参考xv6自己完成并测试(系统时钟取消static以被这里引用)

这部分的实现可以帮助你理解 proc_sleep() 参数为什么要传入锁, 你应该去了解xv6是如何使用 sleep() 的