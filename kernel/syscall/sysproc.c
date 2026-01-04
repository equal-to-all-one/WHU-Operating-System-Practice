#include "proc/cpu.h"
#include "mem/vmem.h"
#include "mem/pmem.h"
#include "mem/mmap.h"
#include "lib/str.h"
#include "lib/print.h"
#include "syscall/syscall.h"
#include "syscall/sysfunc.h"
#include "dev/timer.h"
#include "fs/buf.h"
#include "fs/fs.h"
#include "fs/bitmap.h"
#include "fs/dir.h"
#include "proc/elf.h"
// 堆伸缩
// uint64 new_heap_top 新的堆顶 (如果是0代表查询, 返回旧的堆顶)
// 成功返回新的堆顶 失败返回-1
uint64 sys_brk()
{
    uint64 new_heap_top;
    arg_uint64(0, &new_heap_top);
    proc_t* p = myproc();
    uint64 old_heap_top = p->heap_top;

    if(new_heap_top == 0) {
        // printf("look: heap_top = %p\n", p->heap_top);
        // vm_print(p->pgtbl);
        // printf("\n");
        return old_heap_top;
    }

    if(new_heap_top > old_heap_top) {
        if(uvm_heap_grow(p->pgtbl, old_heap_top, new_heap_top - old_heap_top, PTE_R | PTE_W | PTE_U) == 0) {
            return -1;
        }
        p->heap_top = new_heap_top;
        // printf("grow: heap_top = %p\n", p->heap_top);
    } else if(new_heap_top < old_heap_top) {
        uvm_heap_ungrow(p->pgtbl, old_heap_top, old_heap_top - new_heap_top);
        p->heap_top = new_heap_top;
        // printf("ungrow: heap_top = %p\n", p->heap_top);
    } else {
        // printf("look: heap_top = %p\n", p->heap_top);
    }

    // vm_print(p->pgtbl);
    // printf("\n");

    return p->heap_top;
}

// 内存映射
// uint64 start 起始地址 (如果为0则由内核自主选择一个合适的起点, 通常是顺序扫描找到一个够大的空闲空间)
// uint32 len   范围(字节, 检查是否是page-aligned)
// 成功返回映射空间的起始地址, 失败返回-1
uint64 sys_mmap()
{
    uint64 start;
    uint32 len;
    arg_uint64(0, &start);
    arg_uint32(1, &len);

    if (len == 0 || (start % PGSIZE != 0) || (len % PGSIZE != 0)) {
        return -1;
    }

    proc_t* p = myproc();
    uint32 npages = len / PGSIZE;

    // If start is 0, find a suitable region
    if (start == 0) {
        mmap_region_t* curr = p->mmap;
        while (curr) {
            if (curr->npages >= npages) {
                start = curr->begin;
                break;
            }
            curr = curr->next;
        }
        if (start == 0) return -1; // No suitable region found
    }

    uvm_mmap(start, npages, PTE_R | PTE_W | PTE_U);

    // uvm_show_mmaplist(p->mmap);
    // vm_print(p->pgtbl);
    // printf("\n");

    return start;
}

// 取消内存映射
// uint64 start 起始地址
// uint32 len   范围(字节, 检查是否是page-aligned)
// 成功返回0 失败返回-1
uint64 sys_munmap()
{
    uint64 start;
    uint32 len;
    arg_uint64(0, &start);
    arg_uint32(1, &len);

    if (len == 0 || (start % PGSIZE != 0) || (len % PGSIZE != 0)) {
        return -1;
    }

    proc_t* p = myproc();
    uint32 npages = len / PGSIZE;

    uvm_munmap(start, npages);

    uvm_show_mmaplist(p->mmap);
    vm_print(p->pgtbl);
    printf("\n");

    return 0;
}

// 打印字符
// uint64 addr
uint64 sys_print()
{
    // printf("%s", "sys_print called: ");
    uint64 addr;
    char buf[512];
    arg_uint64(0, &addr);
    proc_t *p = myproc();
    uvm_copyin_str(p->pgtbl, (uint64)buf, addr, sizeof(buf));
    printf("%s", buf);
    return 0;
}

// 进程复制
uint64 sys_fork()
{
    // printf("%s", "sys_fork called: ");
    return proc_fork();
}

// 进程等待
// uint64 addr  子进程退出时的exit_state需要放到这里 
uint64 sys_wait()
{
    // printf("%s", "sys_wait called: ");
    uint64 addr;
    arg_uint64(0, &addr);
    return proc_wait(addr);
}

// 进程退出
// int exit_state
uint64 sys_exit()
{
    // printf("%s", "sys_exit called: ");
    int exit_state;
    arg_uint32(0, (uint32*)&exit_state);
    proc_exit(exit_state);
    return 0;
}

extern timer_t sys_timer;

// 进程睡眠一段时间
// uint32 second 睡眠时间
// 成功返回0, 失败返回-1
uint64 sys_sleep()
{
    // printf("%s", "sys_sleep called: ");
    uint32 sec;
    uint64 ticks0;
    
    arg_uint32(0, &sec);
    if(sec < 0)
        sec = 0;
    spinlock_acquire(&sys_timer.lk);
    ticks0 = sys_timer.ticks;
    while(sys_timer.ticks - ticks0 < sec * 10){
        // if(myproc()->killed){
        //     spinlock_release(&sys_timer.lk);
        //     return -1;
        // }
        proc_sleep(&sys_timer.ticks, &sys_timer.lk);
    }
    spinlock_release(&sys_timer.lk);
    return 0;
}

// 执行一个ELF文件
// char* path
// char** argv 注意传入的最后一个参数必须是NULL
// 成功返回argc 失败返回-1
uint64 sys_exec()
{
    char path[DIR_PATH_LEN];    // 文件路径
    char* argv[ELF_MAXARGS];    // 参数指针数组
    uint64 uargv, uarg;
    int i;

    arg_str(0, path, DIR_PATH_LEN);
    arg_uint64(1, &uargv);

    memset(argv, 0, sizeof(argv));

    for(i = 0; i < ELF_MAXARGS; i++){
        // fetch the pointer to the i-th argument string
        uvm_copyin(myproc()->pgtbl, (uint64)&uarg, uargv + sizeof(uint64) * i, sizeof(uint64));
        
        if(uarg == 0){
            argv[i] = 0;
            break;
        }

        // allocate kernel buffer for the argument string
        argv[i] = (char*)pmem_alloc(false); 
        if(argv[i] == 0)
            goto bad;
        
        // fetch the argument string
        uvm_copyin_str(myproc()->pgtbl, (uint64)argv[i], uarg, PGSIZE); 
    }

    int ret = proc_exec(path, argv);

    for(i = 0; i < ELF_MAXARGS && argv[i] != 0; i++)
        pmem_free((uint64)argv[i], false);

    return ret;

bad:
    for(i = 0; i < ELF_MAXARGS && argv[i] != 0; i++)
        pmem_free((uint64)argv[i], false);
    return -1;
}