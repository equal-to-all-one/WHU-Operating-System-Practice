# LAB-4: 第一个用户态进程的诞生

## 代码组织结构

ECNU-OSLAB
├── **include**
│ ├── dev
│ │ ├── timer.h
│ │ ├── plic.h
│ │ └── uart.h
│ ├── lib
│ │ ├── print.h
│ │ ├── lock.h
│ │ └── str.h
│ ├── proc
│ │ ├── proc.h **(NEW)**
│ │ ├── initcode.h **(NEW)** 自动生成, 无需copy
│ │ └── cpu.h **(CHANGE)** 新增函数和字段声明
│ ├── mem
│ │ └── pmem.h
│ │ └── vmem.h **(CHANGE)** VA_MAX移动到memlayout.h
│ ├── trap
│ │ └── trap.h **(CHANGE)** 新增函数声明
│ ├── common.h
│ ├── memlayout.h **(CHANGE)** 新增一些定义
│ └── riscv.h
├── **kernel**
│ ├── boot
│ │ ├── main.c **(CHANGE)** 惯例更新
│ │ ├── start.c
│ │ ├── entry.S
│ │ └── Makefile
│ ├── dev
│ │ ├── uart.c
│ │ ├── timer.c
│ │ ├── plic.c
│ │ └── Makefile
│ ├── lib
│ │ ├── print.c
│ │ ├── spinlock.c
│ │ ├── str.c
│ │ └── Makefile
│ ├── proc
│ │ ├── cpu.c **(CHANGE)** 新增myproc()
│ │ ├── proc.c **(TODO)**
│ │ ├── swtch.S **(NEW)**
│ │ └── Makefile
│ ├── mem
│ │ ├── pmem.c
│ │ ├── kvm.c **(CHANGE)** 在kvm_init()里增加trampoline和kstack映射
│ │ └── Makefile
│ ├── trap
│ │ ├── trap_kernel.c **(CHANGE)** 删去数组的static标志
│ │ ├── trap_user.c **(TODO)**
│ │ ├── trap.S
│ │ ├── trampoline.S **(NEW)**
│ │ └── Makefile
│ ├── Makefile
│ └── kernel.ld **(CHANGE)** 支持trampsec
├── **user**
│ ├── syscall_arch.h **(NEW)**
│ ├── syscall_num.h **(NEW)**
│ ├── sys.h **(NEW)**
│ ├── initcode.c **(NEW)** 第一个用户态进程的代码
│ └── Makefile **(NEW)**
├── Makefile **(CHANGE)** 新增user相关支持
└── common.mk

**标记说明**

1. **TODO** 本次新增的文件 + 其中有函数待实现 (核心部分)
2. **NEW** 本次新增的文件 + 无需做修改 (辅助部分)
3. **CHANGE** 本来就有的文件 + 需要做修改 或 助教做了修改 (整体兼容)

**重要提醒: 本次实验不需要任何助教未定义的函数! 不要私自复制xv6里的函数(如申请进程)!**

## 任务一: 第一个进程的定义和初始化

首先让我们回顾一下前三次实验做了什么:

lab-1 实现了机器启动, 从M-mode的start()到S-mode的main(), 可以向屏幕输出内容

lab-2 实现了基本的内存管理系统, 包括物理页维护和内核虚拟页映射, 可以开启分页系统

lab-3 实现了基本的内核陷阱处理, 了解了中断和异常的基本概念, 实现了时钟中断和UART输入中断

这次我们希望开启新的模块: 进程模块 (涉及的代码文件: **main.c cpu.c proc.c swtch.S kvm.c**)

### 进程的定义

按照惯例, 我们先考虑最简单的情况: 整个系统只有一个进程, 不需要考虑进程调度和进程状态的问题

那么这个最简单的 **proczero** 应该具备哪些字段呢?

```
// 进程定义
typedef struct proc {

    int pid;                 // 标识符

    pgtbl_t pgtbl;           // 用户态页表
    uint64 heap_top;         // 用户堆顶(以字节为单位)
    uint64 ustack_pages;     // 用户栈占用的页面数量
    trapframe_t* tf;         // 用户态内核态切换时的运行环境暂存空间

    uint64 kstack;           // 内核栈的虚拟地址
    context_t ctx;           // 内核态进程上下文

} proc_t;
```

首先是一个全局的标识符, 它是进程的 "名字"

然后是四个用户态的字段, 分别是:

- 页表(地址空间管理)
- 堆和栈的记录信息
- trapframe(寄存器信息和内核信息)

最后是两个内核态的字段, 分别是:

- 内核栈地址(内核不需要堆空间)
- 内核态上下文(寄存器信息)

其他字段我们暂时用不到, 后面用到再添加

### 上下文与上下文切换

定义完进程后我们需要在 **cpu.h** 里新增两个字段:

- cpu 运行的进程 (用户进程)
- cpu 的上下文

这时候会产生一个问题: 为什么 cpu 和进程都有一个上下文?

首先考虑, 在第一个用户进程诞生之前, 为什么没有上下文的概念?

因为操作系统独占CPU的寄存器, 没有别人和他抢占, 所以不需要一个名为上下文的内存区域暂存各个寄存器的值

此时的操作系统可以理解为一个高级进程(它是隐式的), 因为操作系统毫无疑问也是程序, 那它的栈在哪?

```
__attribute__ ((aligned (16))) uint8 CPU_stack[PGSIZE * NCPU];
```

还记得我们在 **start.c** 中定义的`CPU_stack`吗, 它就是这些高级进程的栈空间

现在我们有新的进程了, 它叫用户进程, 也可以叫低级进程

于是可能发生CPU的寄存器抢占, 高级进程和低级进程都需要一个 **上下文 context_t**

作为抢占发生时自己的运行环境(也就是通用寄存器的值)的暂存空间(位于内存)

当被抢占的进程夺回CPU控制权时, 把这部分存在内存里的值写回各个寄存器, 就可以接着执行了

这就是所谓 "运行环境的保存和恢复", 也是 **swtch.S** 做的事情

高级进程每个CPU只有一个, 所以它的上下文保存在 `cpu_t` 里

低级进程可能很多, 所以上下文保存在 `proc_t` 里

### trapframe的定义

`trapframe` 的定义和 `context` 有些类似, 看起来都是一堆寄存器信息

区别在于 `context` 是内核态进程切换的暂存区, `trapframe` 是同一个进程在内核态和用户态切换的暂存区

`trapframe` 里的字段可以分成两部分去看:

第一部分是: kernel_xxx 的内核信息 + epc 模式切换的返回地址

第二部分是: 通用寄存器信息(比 `context` 更全, 模式间切换 比 同模式内切换 需要保存的信息更多)

哪些寄存器是我们需要特别设置的呢? 第一部分的全部 + 第二部分的栈顶寄存器sp

### proc.c 函数填写

```
pgtbl_t  proc_pgtbl_init(uint64 trapframe);
void     proc_make_fisrt();
```

在 **main.c** 的最后, CPU0 会调用 `proc_make_first` 创建第一个进程

`proc_make_first` 需要做的事情可以分成三部分:

- 准备用户态页表 (完成一系列映射以实现注释里的用户地址空间, 需要调用 `proc_pgtbl_init`)

  注意: 需要在kvm_init里增加trampoline和kstack(需要pmem_alloc, 使用KSTACK宏定义)的映射

- 设置 **proczero** 的各个字段 (trapframe里需要设置epc和sp, context里需要设置ra和sp)

- 设置 CPU 当前运行的进程为 **proczero**, 使用 `swtch` 切换进程上下文 (CPU的ctx 到 proczero的ctx)

tips-1: 假设栈的地址空间是 [A, A+PGSIZE), 那么栈的指针初始应该设置为 A+PGSIZE 而不是 A (sp是向下运动的)

tips-2: 关心 **initcode.h** 是如何在 **user** 目录中编译链接生成的, 以及 **inicode.c** 做了什么事

顺利执行的话, 执行流会来到 `trap_user_return`, 这就引出了我们的第二个任务, **用户态陷阱** 的支持

## 任务二: 用户态陷阱处理

### trap流程

回忆一下上一个实验, 我们已经有了内核态陷阱处理的经验

内核态陷阱处理流程是: `kerne_vector`前半部分->`trap_kernel_handler`->`kernel_vector`后半部分

用户态陷阱处理流程类似: `user_vector`->`trap_user_handler`->`trap_user_return`->`user_return`

很明显, 用户态trap处理被拆分成了上下两部分, 为什么不把`trap_user_handler`和`trap_user_return`合并呢?

原因是这样的处理流程假设用户进程一开始就是在用户态的, 但是事实上用户进程出生在内核态

于是初生的用户进程进入用户态的路径是: `proc_make_first`->`trap_user_return`->`user_return`-> U-mode

所以必须把trap处理函数分成两部分以兼容这条分支路径

### trap实现

trap的实现只需关注 **trampoline.S** 和 **trap_user.c** 这两个文件即可

这两个文件里共有四个函数, 分别是上一节里提到的用户态陷阱处理流程的四个阶段

汇编部分的代码已经写好且有完整注释, 需要大家阅读理解

`trap_user_handler` 的实现与 `trap_kernel_handler` 非常类似, 大部分代码可以直接copy

`trap_user_return` 的实现涉及很多寄存器和 **trapframe** 的操作, 确保你完全理解xv6的实现后完成

核心要义: 处理流程是前后照应的, 找到并关注那些前后对应的操作

另外, 在 **initcode.c** 里, **proczero** 向操作系统发出了第一声啼哭

也就是第一个系统调用请求, 你需要在 `trap_user_handler` 中进行响应

响应方式很简单 `printf("get a syscall from proc %d\n", myproc()->pid);`

由于我们的 syscall 机制还不完善, 所以只需做最简单的响应, 切勿画蛇添足!

你还需要简单测试用户态trap是否能和内核态trap一样正常工作

## 总结

至此, 你应该明白第一个用户态进程是如何一步步构建起来的

此外, 你还应该明白用户态trap处理是如何与进程的用户态内核态切换(尤其是syscall)联合起来的

最后, 你应该明白用户态trap和内核态trap的全部处理流程以及它们的异同点

当第一个进程建立起来, 用户态和内核态的trap完全建立, 我们就可以做一些更复杂的工作了

lab-4结束后, 你应当做一个阶段性整理以确保你完全理解目前你写的所有代码, 同时做更多代码正确性测试

后面我们将围绕进程这一概念, 完善进程管理模块和用户虚拟内存管理模块, 而trap的故事将告一段落