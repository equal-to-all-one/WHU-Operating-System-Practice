#include "proc/cpu.h"
#include "mem/vmem.h"
#include "mem/pmem.h"
#include "mem/mmap.h"
#include "lib/str.h"
#include "lib/print.h"
#include "syscall/sysfunc.h"
#include "syscall/syscall.h"

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
        printf("look: heap_top = %p\n", p->heap_top);
        vm_print(p->pgtbl);
        printf("\n");
        return old_heap_top;
    }

    if(new_heap_top > old_heap_top) {
        if(uvm_heap_grow(p->pgtbl, old_heap_top, new_heap_top - old_heap_top) == 0) {
            return -1;
        }
        p->heap_top = new_heap_top;
        printf("grow: heap_top = %p\n", p->heap_top);
    } else if(new_heap_top < old_heap_top) {
        uvm_heap_ungrow(p->pgtbl, old_heap_top, old_heap_top - new_heap_top);
        p->heap_top = new_heap_top;
        printf("ungrow: heap_top = %p\n", p->heap_top);
    } else {
        printf("look: heap_top = %p\n", p->heap_top);
    }

    vm_print(p->pgtbl);
    printf("\n");

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

    uvm_show_mmaplist(p->mmap);
    vm_print(p->pgtbl);
    printf("\n");

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

// copyin 测试 (int 数组)
// uint64 addr
// uint32 len
// 返回 0
uint64 sys_copyin()
{
    proc_t* p = myproc();
    uint64 addr;
    uint32 len;

    arg_uint64(0, &addr);
    arg_uint32(1, &len);

    int tmp;
    for(int i = 0; i < len; i++) {
        uvm_copyin(p->pgtbl, (uint64)&tmp, addr + i * sizeof(int), sizeof(int));
        printf("get a number from user: %d\n", tmp);
    }

    return 0;
}

// copyout 测试 (int 数组)
// uint64 addr
// 返回数组元素数量
uint64 sys_copyout()
{
    int L[5] = {1, 2, 3, 4, 5};
    proc_t* p = myproc();
    uint64 addr;

    arg_uint64(0, &addr);
    uvm_copyout(p->pgtbl, addr, (uint64)L, sizeof(int) * 5);

    return 5;
}

// copyinstr测试
// uint64 addr
// 成功返回0
uint64 sys_copyinstr()
{
    char s[64];

    arg_str(0, s, 64);
    printf("get str from user: %s\n", s);

    return 0;
}

// 测试页表复制和销毁
uint64 sys_test_vm()
{
    proc_t* p = myproc();
    
    printf("Original Page Table:\n");
    vm_print(p->pgtbl);
    
    // Allocate a new trapframe for the new process (simulated)
    trapframe_t* new_tf = (trapframe_t*)pmem_alloc(true);
    if (new_tf == NULL) panic("sys_test_vm: alloc tf failed");
    memset(new_tf, 0, PGSIZE);

    // Initialize new page table
    pgtbl_t new_pgtbl = proc_pgtbl_init((uint64)new_tf);
    if (new_pgtbl == NULL) panic("sys_test_vm: init pgtbl failed");
    
    printf("\nCopying Page Table...\n");
    uvm_copy_pgtbl(p->pgtbl, new_pgtbl, p->heap_top, p->ustack_pages, p->mmap);
    
    printf("New Page Table:\n");
    vm_print(new_pgtbl);
    
    // Verify Deep Copy
    // Check initcode at CODE_TEXT_START
    uint64 test_va = CODE_TEXT_START; 
    pte_t* old_pte = vm_getpte(p->pgtbl, test_va, false);
    pte_t* new_pte = vm_getpte(new_pgtbl, test_va, false);
    
    if (old_pte && new_pte && (*old_pte & PTE_V) && (*new_pte & PTE_V)) {
        uint64 old_pa = PTE_TO_PA(*old_pte);
        uint64 new_pa = PTE_TO_PA(*new_pte);
        printf("Verification at VA %p:\n", test_va);
        printf("Old PA: %p\n", old_pa);
        printf("New PA: %p\n", new_pa);
        
        if (old_pa != new_pa) {
            printf("Deep Copy Verified: Physical addresses are different.\n");
            
            bool match = true;
            uint8* p1 = (uint8*)old_pa;
            uint8* p2 = (uint8*)new_pa;
            for(int i=0; i<PGSIZE; i++) {
                if(p1[i] != p2[i]) {
                    match = false;
                    break;
                }
            }

            if (match) {
                printf("Content Verified: Content is identical.\n");
            } else {
                printf("Content Mismatch!\n");
            }
        } else {
            printf("Deep Copy Failed: Physical addresses are same!\n");
        }
    } else {
        printf("Verification Failed: PTE not valid at %p.\n", test_va);
    }

    printf("\nDestroying New Page Table...\n");
    uvm_destroy_pgtbl(new_pgtbl);
    
    // Free the trapframe manually as uvm_destroy_pgtbl unmaps it but doesn't free it
    pmem_free((uint64)new_tf, true);
    
    printf("Destroyed.\n");
    
    return 0;
}
