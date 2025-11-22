# LAB-2: 内存管理初步

在LAB-1中我们已经完成了机器启动并使用uart向控制台打印字符

理解了OS如何在RISC-V体系结构和QEMU模拟器的帮助下进入main函数

接下来我们要完成内存管理的初步设计

首先完成物理内存管理设计, 然后完成一部分虚拟内存管理设计

## 代码组织结构

ECNU-OSLAB  
├── include  
│   ├── dev  
│   │   └── uart.h    
│   ├── lib  
│   │   ├── print.h  
│   │   ├── lock.h  
│   │   └── str.h **(NEW)**  
│   ├── proc  
│   │   └── cpu.h  
│   ├── mem **(NEW)**  
│   │   └── pmem.h  
│   │   └── vmem.h  
│   ├── common.h **(CHANGE)**  
│   ├── memlayout.h **(CHANGE)**  
│   └── riscv.h **(CHANGE)**  
├── kernel  
│   ├── boot  
│   │   ├── main.c **(CHANGE)**   
│   │   ├── start.c  
│   │   ├── entry.S  
│   │   └── Makefile  
│   ├── dev  
│   │   ├── uart.c  
│   │   └── Makefile  
│   ├── lib  
│   │   ├── print.c  
│   │   ├── spinlock.c  
│   │   ├── str.c **(NEW)**   
│   │   └── Makefile    
│   ├── proc  
│   │   ├── cpu.c  
│   │   └── Makefile  
│   ├── mem  **(NEW)**  
│   │   ├── pmem.c  **(TODO)**  
│   │   ├── kvm.c  **(TODO)**  
│   │   └── Makefile  
│   ├── Makefile  
│   └── kernel.ld **(CHANGE)**  
├── Makefile  
└── common.mk  

**CHANGE** 说明:

1. **main.c** 这个文件每次lab基本都会变化

2. **riscv.h** 最底下的与虚拟内存相关的部分删去

3. **memlayout.h** 新增一些硬件寄存器地址

4. **common.h** 新增PGSIZE定义

5. **kernel.ld** 新增一些`PROVIDE`用于标记关键地址

## 第一阶段实验：物理内存的分配与回收

首先需要关注的文件是 **kernel.ld** 文件, 它规定了内核文件 **kernel-qemu** 在载入内存时的布局

我们可以粗略把物理内存分成三个部分：

- **0x80000000 ~ KERNEL_DATA** 存放了 **内核代码**

- **KERNEL_DATA ~ ALLOC_BEGIN** 存放了 **内核数据**

- **ALLOC_BEGIN ~ ALLOC_END** 属于 **未使用可分配的物理页**

如果你想进一步了解可执行文件(ELF)的布局信息可以自行查阅资料

很明显, 前两部分的物理页会一直被内核占用, 不会动态分配和回收

我们需要管理的就是第三部分的物理页（管理对象）

所谓物理页的管理我们只用实现三个简单的函数（管理方法）

```
void  pmem_init();   // 初始系统, 只调用一次
void* pmem_alloc();  // 申请可用的物理页
void  pmem_free();   // 释放申请了的物理页
```

这三个函数体现了经典的共享资源管理方法：初始化资源, 占有资源, 释放资源

首先我们在 **common.h** 中定义物理页的大小为 **PGSIZE (4096 Byte)**

这样问题就从管理连续的地址空间 **ALLOC_BEGIN ~ ALLOC_END** 转化成了

管理有限个独立的物理页, 我们选择使用链表作为核心的数据结构(在 **pmem.c** 中)

```
// 物理页节点
typedef struct page_node {
    struct page_node* next;
} page_node_t;

// 许多物理页构成一个可分配的区域
typedef struct alloc_region {
    uint64 begin;          // 起始物理地址
    uint64 end;            // 终止物理地址
    spinlock_t lk;         // 自旋锁(保护下面两个变量)
    uint32 allocable;      // 可分配页面数    
    page_node_t list_head; // 可分配链的链头节点
} alloc_region_t;

// 内核和用户可分配的物理页分开
static alloc_region_t kern_region, user_region;
```

在每个 **allocable page(4096 Byte)** 的起始 **8 Byte** 存储一个指针, 它指向下一个 **page**

这样最简单的链式结构就形成了, 我们只需要给一个固定的链表头 **list_head**

构成这样的结构: list_head->page A->page B->page C->.......-> NULL

那么`alloc`操作就是移除 list_head->next, `free`操作就是将 page 插入list_head->next (头插法)

如果你对数据结构里的单链表有所了解, 这一点应该不难实现

考虑以下问题：

内核和用户都会申请和使用物理页, 如果有一个恶意的用户程序, 不断申请物理页且从不释放

那么当内核需要申请物理页时就会发现物理页耗尽了, 内核被用户攻击卡死是不可接受的

我们需要将内核物理页和用户物理页分开, 定义 **KERNEL_PAGES** 作为内核占用的物理页数目

于是 **ALLOC_BEGIN ~ ALLOC_END** 被分成两部分

- **ALLOC_BEGIN ~ ALLOC_BEGIN + KERNEL_PAGES * PGSIZE** 内核使用

- **ALLOC_BEGIN + KERNEL_PAGES * PGSIZE ~ ALLOC_END** 用户使用

使用 `kern_region, user_region` 分别管理, `alloc` 和 `free` 操作需要注明是否 **in_kernel**

这也解释了为什么 **alloc_region_t** 里需要记录这个内存区域的起点和终点

讲到这里, 你应该理解你需要做的事情了, 动手开始做吧

另外, 在 **kernel/lib/str.h** 里我们新增了三个辅助函数, 能简化一些操作

## 第一阶段测试

```
volatile static int started = 0;

volatile static int over_1 = 0, over_2 = 0;

static int* mem[1024];

int main()
{
    int cpuid = r_tp();

    if(cpuid == 0) {

        print_init();
        pmem_init();

        printf("cpu %d is booting!\n", cpuid);
        __sync_synchronize();
        started = 1;

        for(int i = 0; i < 512; i++) {
            mem[i] = pmem_alloc(true);
            memset(mem[i], 1, PGSIZE);
            printf("mem = %p, data = %d\n", mem[i], mem[i][0]);
        }
        printf("cpu %d alloc over\n", cpuid);
        over_1 = 1;
        
        while(over_1 == 0 || over_2 == 0);
        
        for(int i = 0; i < 512; i++)
            pmem_free((uint64)mem[i], true);
        printf("cpu %d free over\n", cpuid);

    } else {

        while(started == 0);
        __sync_synchronize();
        printf("cpu %d is booting!\n", cpuid);
        
        for(int i = 512; i < 1024; i++) {
            mem[i] = pmem_alloc(true);
            memset(mem[i], 1, PGSIZE);
            printf("mem = %p, data = %d\n", mem[i], mem[i][0]);
        }
        printf("cpu %d alloc over\n", cpuid);
        over_2 = 1;

        while(over_1 == 0 || over_2 == 0);

        for(int i = 512; i < 1024; i++)
            pmem_free((uint64)mem[i], true);
        printf("cpu %d free over\n", cpuid);        
 
    }
    while (1);    
}
```

我们提供了一个测试样例, 它的作用是：

cpu-0 和 cpu-1 并行申请内核中的全部物理内存, 赋值并输出信息

待申请全部结束, 并行释放所有申请的物理内存

理想的输出结果可能是这样的：

![pic](./picture/pmem.png)

很明显, 一个测试用例是不够的, 你应当自己写一些别的测试用例

建议测试的方向：

1. 尝试触发你在 `pmem_alloc` 和 `pmeme_free` 中设置的 `panic` 或 `assert`

2. 用户地址空间 `pmem_alloc` 和 `pmem_free` 操作的正确性

3. 任何你能想到的可能出错的地方

**为什么要这么做?**

因为你后面需要依赖自己写的函数, 如果现在没发现错误, 未来Debug会更困难

所以每个模块写完后都要及时测试, 对你的代码负责

另外值得强调的一点是：学会使用`panic` 和 `assert` 做必要检查

在出问题前输出有价值的错误信息, 比系统直接卡死或进入错误状态, 更容易Debug

这种理论又叫**防御性编程**, 对输入参数保持警惕, 充分检查, 确保错误不会传递

这件事非常重要, 后面不多强调, 需要从现在起培养习惯

## 第二阶段: 内核态虚拟内存管理

在开始这部分代码的编写前，你应该先阅读 **vmem.h** 里面的注释

注释里介绍了虚拟内存的一个规范 **SV39** 即 39 bit 虚拟地址的虚拟内存

之所以要遵守这个规范，是为了能在 risc-v 体系结构的机器上正常使用 MMU

注释里的信息是我们会用到的部分, 其余部分可以在risc-v手册中自行查看

之后我们要关注两个核心概念：**页表项(PTE)** 和 **页表(pgtbl)**

```
// 页表项
typedef uint64 pte_t;

// 顶级页表
typedef uint64* pgtbl_t;
```

页表是由页表项构成的，你可以理解成数组和数组里元素的关系

一个页表项对应一个物理页，页表项主要由两部分组成：

- 它所管理的物理页的物理页号 (PPN字段)

- 它锁管理的物理页的标志位 (低10bit)

页表的结构是由三级页表构成的, 可以理解为高度为3的树

举一个简单的例子理解它的结构:

假设有一个 6 bit 虚拟地址(虚拟页号) 0x011100 希望找到它对应的物理页号

顶级页表说: 你的前两位是 01 = 1, 那么我的四个下属(0,1,2,3)里面, 1号下属可以满足你的需要

1号次级页表说: 来我这的虚拟地址前2位都是01, 不用看, 你的中间2位是11, 推荐你去我的3号下属

3号低级页表说: 来我这的虚拟地址前4位都是0111, 你的最后2位是00, 推荐你去我的0号下属

低级页表的下属是谁呢？ 没错, 就是非页表的物理页

这里需要做一个区分:

页表本身也是存储在物理页中, 我们称这种物理页叫页表物理页, 特点是它的 PTE_R PTE_W PTE_X 都是0

听起来很不可思议吧，这个物理页不能读不能写不能执行? 其实这只是risc-v的规定, 不必深究

总而言之，我们能从**顶级页表**的PTE里获得**次级页表**所在的物理页的物理地址和标志位

我们能从**次级页表**的PTE里获得**低级页表**所在的物理页的物理地址和标志位

我们能从**低级页表**的PTE里获得**非页表物理页**(真正存储数据和代码)的物理页号和标志位

到此为止，你应该对页表和页表项建立起基本的认识了

认识完数据结构后，我们考虑如何管理这个数据结构，也就是非常经典的 `vm_mappages`

所谓地址映射，本质就是对页表中某些页表项的修改（或增删）

大部分是设置它存储的物理页号, 有时也会设置或修改它的标志位(比如从 PTE_R 改成 PTE_R | PTE_W)

关于地址映射，最简单的想法肯定是这样的：

```
typedef struct {
    uint64 va; // 虚拟地址
    uint64 pa; // 物理地址
} addr_pair_t;
addr_pair_t pgtbl[PAIR_NUM];
```

做一个简单的一一对应，查询映射关系只需遍历数组，删除和增加映射关系也很容易

考虑到时间开销，第二个版本可能是这样的：

```
typedef struct {
    uint64 pa; // 物理地址
} addr_t;
addr_t pgtbl[VA_NUM];
```

开一个巨长的数组，数组长度等于所有虚拟页的数量（比如支持4GB虚拟内存，就是4GB/4KB）

这样我就能用O(1)的复杂度快速查询了，但是代价是巨大的空间浪费

于是有了第三个版本：空间和时间的折中

由于巨大的虚拟内存空间很难被完全使用，有时候只是使用最高处和最低处两部分

所以可以使用树形的三级映射来节约空间，同时时间开销也是O(1)级别的，虽然比不上数组法的O(1)

聊完背景之后简单说明你需要做的事情, **vmem.c** 中函数实现顺序如下

`vm_getpte -> vm_mappages -> vm_unmappages`

提示: 实现过程中可以使用 **vmem.h** 里面的宏定义

核心: 理解页表的构成(页表项与三级映射)和页表操作(映射与解映射)

我们提供了一个vm_print函数，它可以输出整个页表中的有用信息，用于Debug

构建基本的页表操作函数后，我们需要给内核页表 **kernel_pgtbl** 填一些东西并正式使用

`kvm_init -> kvm_inithart`

**kernel_pgtbl** 的映射大致可以划分成两部分：

一部分是硬件寄存器区域，这部分的物理地址不是我们可以使用的，可以理解为QEMU保留的"假地址"

另一部分是可用内存区域，即0x8000-0000 到 0x8000-0000 + 128 MB

内核页表对这两部分的映射都是虚拟地址等于物理地址的直接映射

映射完毕后我们的 kernel_pgtbl 就可以上线工作了，把它写入**satp**寄存器即可正式开启**MMU**翻译

我们终于结束了直接访问物理地址的时代，此后的所有地址访问，只要**satp**寄存器里不是 0

本质都是访问虚拟地址，因为 **MMU** 会忠实地将所有地址访问 va 经过页表翻译变成 pa

那为什么我们代码里还是使用 pa 和 物理地址这种概念呢

因为我们是人类，我们知道 kernel_pgtbl 是直接映射的，虚拟地址就等于物理地址

举个例子, 我们有一个物理地址 0x80000000, 指针p指向它

*p = 999 完成了一次赋值, 向这个物理页里写入了 999

实际的过程其实是 MMU 根据内核页表的信息查找 **虚拟地址** 0x80000000 对应的物理地址

发现是 0x80000000，然后向这个**物理地址**里写入了 999 (查询过程是隐式的，硬件层面执行很快)

记住程序员视角和MMU视角这两种视角，否则你会感到很疑惑

## 第二阶段测试

```
int main()
{
    int cpuid = r_tp();

    if(cpuid == 0) {

        print_init();
        pmem_init();
        kvm_init();
        kvm_inithart();

        printf("cpu %d is booting!\n", cpuid);
        __sync_synchronize();
        // started = 1;

        pgtbl_t test_pgtbl = pmem_alloc(true);
        uint64 mem[5];
        for(int i = 0; i < 5; i++)
            mem[i] = (uint64)pmem_alloc(false);

        printf("\ntest-1\n\n");    
        vm_mappages(test_pgtbl, 0, mem[0], PGSIZE, PTE_R);
        vm_mappages(test_pgtbl, PGSIZE * 10, mem[1], PGSIZE / 2, PTE_R | PTE_W);
        vm_mappages(test_pgtbl, PGSIZE * 512, mem[2], PGSIZE - 1, PTE_R | PTE_X);
        vm_mappages(test_pgtbl, PGSIZE * 512 * 512, mem[2], PGSIZE, PTE_R | PTE_X);
        vm_mappages(test_pgtbl, VA_MAX - PGSIZE, mem[4], PGSIZE, PTE_W);
        vm_print(test_pgtbl);

        printf("\ntest-2\n\n");    
        vm_mappages(test_pgtbl, 0, mem[0], PGSIZE, PTE_W);
        vm_unmappages(test_pgtbl, PGSIZE * 10, PGSIZE, true);
        vm_unmappages(test_pgtbl, PGSIZE * 512, PGSIZE, true);
        vm_print(test_pgtbl);

    } else {

        while(started == 0);
        __sync_synchronize();
        printf("cpu %d is booting!\n", cpuid);
         
    }
    while (1);    
}
```

这段测试代码实际测试了两件事情：

1. 使用内核页表后你的程序是否还能正常执行

2. 使用映射和解映射操作修改你的页表，使用vm_print输出它被修改前后的对比

理想结果如下：

![图片](./picture/vmem.png)

你应当补充更多你能想到的测试，确保你的函数不会在以后出问题

这次实验实际在`kvm_init`里埋下了一些伏笔，那些硬件寄存器映射我们还没用起来

在下一次的中断异常实验中我们会用上这次的映射