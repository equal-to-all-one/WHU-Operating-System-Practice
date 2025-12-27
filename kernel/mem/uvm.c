#include "mem/mmap.h"
#include "mem/pmem.h"
#include "mem/vmem.h"
#include "proc/cpu.h"
#include "lib/print.h"
#include "lib/str.h"
#include "memlayout.h"

// 连续虚拟空间的复制(在uvm_copy_pgtbl中使用)
// 从 old 页表复制 [begin, end) 范围到 new 页表
// 包括物理页的申请和内容的拷贝
static void copy_range(pgtbl_t old, pgtbl_t new, uint64 begin, uint64 end)
{
    uint64 va, pa, page;
    int flags;
    pte_t* pte;

    for(va = begin; va < end; va += PGSIZE)
    {
        pte = vm_getpte(old, va, false);
        assert(pte != NULL, "uvm_copy_pgtbl: pte == NULL");
        assert((*pte) & PTE_V, "uvm_copy_pgtbl: pte not valid");
        
        pa = (uint64)PTE_TO_PA(*pte);
        flags = (int)PTE_FLAGS(*pte);

        page = (uint64)pmem_alloc(false);
        memmove((char*)page, (const char*)pa, PGSIZE);
        vm_mappages(new, va, page, PGSIZE, flags);
    }
}

// 两个 mmap_region 区域合并
// 保留一个 释放一个 不操作 next 指针
// 在uvm_munmap里使用
static void mmap_merge(mmap_region_t* mmap_1, mmap_region_t* mmap_2, bool keep_mmap_1)
{
    // 确保有效和紧临
    assert(mmap_1 != NULL && mmap_2 != NULL, "mmap_merge: NULL");
    assert(mmap_1->begin + mmap_1->npages * PGSIZE == mmap_2->begin, "mmap_merge: check fail");
    
    // merge
    if(keep_mmap_1) {
        mmap_1->npages += mmap_2->npages;
        mmap_region_free(mmap_2);
    } else {
        mmap_2->begin -= mmap_1->npages * PGSIZE;
        mmap_2->npages += mmap_1->npages;
        mmap_region_free(mmap_1);
    }
}

// 打印以 mmap 为首的 mmap 链
// for debug
void uvm_show_mmaplist(mmap_region_t* mmap)
{
    mmap_region_t* tmp = mmap;
    printf("\nmmap allocable area:\n");
    if(tmp == NULL)
        printf("NULL\n");
    while(tmp != NULL) {
        printf("allocable region: %p ~ %p\n", tmp->begin, tmp->begin + tmp->npages * PGSIZE);
        tmp = tmp->next;
    }
}

// 递归释放 页表占用的物理页 和 页表管理的物理页
// ps: 顶级页表level = 2, level = 0 是三级页表
static void destroy_pgtbl(pgtbl_t pgtbl, uint32 level)
{
    for (int i = 0; i < 512; i++) {
        pte_t pte = pgtbl[i];
        if (pte & PTE_V) {
            uint64 pa = PTE_TO_PA(pte);
            if (level > 0) {
                destroy_pgtbl((pgtbl_t)pa, level - 1);
                pmem_free(pa, true); // Page table pages are kernel memory
            } else {
                // Leaf page (User memory)
                pmem_free(pa, false); // User memory
            }
            pgtbl[i] = 0;
        }
    }
}

// 页表销毁：trapframe 和 trampoline 单独处理
void uvm_destroy_pgtbl(pgtbl_t pgtbl)
{
    // Unmap trampoline and trapframe to avoid freeing them in recursive destroy
    // trampoline 是全局唯一的，不属于某个进程的页表
    vm_unmappages(pgtbl, TRAMPOLINE, PGSIZE, false);
    // trapframe 是属于进程的，我们在这里只是删除页表，没有释放进程，不能释放
    vm_unmappages(pgtbl, TRAPFRAME, PGSIZE, false);
    
    // Recursive destroy (root level is 2 for Sv39)
    destroy_pgtbl(pgtbl, 2);
    
    // Free the root page table itself
    pmem_free((uint64)pgtbl, true);
}

// 拷贝页表 (拷贝并不包括trapframe 和 trampoline)
void uvm_copy_pgtbl(pgtbl_t old, pgtbl_t new, uint64 heap_top, uint32 ustack_pages, mmap_region_t* mmap)
{
    /* USER_BASE ~ heap_top */
    // Assuming USER_BASE is CODE_TEXT_START (initcode start)
    // But heap_top is the end of the heap.
    // We should copy from CODE_TEXT_START to heap_top.
    copy_range(old, new, CODE_TEXT_START, heap_top);

    /* ustack */
    uint64 ustack_top = TRAPFRAME;
    uint64 ustack_bottom = ustack_top - ustack_pages * PGSIZE;
    copy_range(old, new, ustack_bottom, ustack_top);

    /* mmap_region */
    uint64 cur = MMAP_BEGIN; 
    mmap_region_t* node = mmap; // node 指向空闲链表的头部
    while (node) {
        // 如果当前空闲块的起始地址(node->begin) 大于 游标(cur)
        // 说明 [cur, node->begin) 这段区间是“非空闲”的，也就是“已分配”的
        if (node->begin > cur) {
            // 复制这段已分配的内存区域
            copy_range(old, new, cur, node->begin);
        }
        
        // 跳过当前的空闲块
        // 将游标 cur 直接移动到当前空闲块的结束位置
        cur = node->begin + node->npages * PGSIZE;
        
        node = node->next;
    }
}

// // 在用户页表和进程mmap链里 新增mmap区域 [begin, begin + npages * PGSIZE)
// 页面权限为perm
void uvm_mmap(uint64 begin, uint32 npages, int perm)
{
    if(npages == 0) return;
    assert(begin % PGSIZE == 0, "uvm_mmap: begin not aligned");

    proc_t* p = myproc();
    mmap_region_t* prev = NULL;
    mmap_region_t* curr = p->mmap;
    uint64 end = begin + npages * PGSIZE;
    uint64 curr_end = 0;

    // ----找到 [begin, end)所在的区域----
    while (curr) {
        curr_end = curr->begin + curr->npages * PGSIZE;
        if (begin >= curr->begin && end <= curr_end) {
            break;
        }
        prev = curr;
        curr = curr->next;
    }

    if (!curr) {
        panic("uvm_mmap: no suitable region found");
    }

    // ----- 切分区域 -----

    // Case 1: 大小完全匹配
    if (begin == curr->begin && end == curr_end) {
        // --- 1.1：头结点 ---
        if (prev) {
            prev->next = curr->next;
        } else { // --- 1.2：非头结点 ---
            p->mmap = curr->next;
        }
        mmap_region_free(curr);
    }
    // Case 2: 开头相同，结尾不同
    else if (begin == curr->begin) {
        curr->begin = end;
        curr->npages -= npages;
    }
    // Case 3: 结尾相同，开头不同
    else if (end == curr_end) {
        curr->npages -= npages;
    }
    // Case 4: 中间区域，包含但不涉及边界
    else {
        mmap_region_t* new_node = mmap_region_alloc();
        if (!new_node) panic("uvm_mmap: alloc failed");
        
        new_node->begin = end;
        new_node->npages = (curr_end - end) / PGSIZE;
        new_node->next = curr->next;
        
        curr->npages = (begin - curr->begin) / PGSIZE;
        curr->next = new_node;
    }

    // ----- 映射物理页 -----
    for (uint64 va = begin; va < end; va += PGSIZE) {
        void* pa = pmem_alloc(false);
        if (!pa) panic("uvm_mmap: pmem alloc failed");
        if (vm_mappages(p->pgtbl, va, (uint64)pa, PGSIZE, perm) != 0) {
            panic("uvm_mmap: mappages failed");
        }
    }
}

// 在用户页表和进程mmap链里释放mmap区域 [begin, begin + npages * PGSIZE)
void uvm_munmap(uint64 begin, uint32 npages)
{
    if(npages == 0) return;
    assert(begin % PGSIZE == 0, "uvm_munmap: begin not aligned");

    proc_t* p = myproc();
    // uint64 end = begin + npages * PGSIZE;

    // ----- 解除映射并释放物理页 -----
    vm_unmappages(p->pgtbl, begin, npages * PGSIZE, true);

    // ----- 在 mmap 链表中插入新的可用区域 -----
    mmap_region_t* new_node = mmap_region_alloc();
    if (!new_node) panic("uvm_munmap: alloc failed");
    new_node->begin = begin;
    new_node->npages = npages;
    new_node->next = NULL;

    // --- 插入有序的mmap链表并尝试合并 ---
    mmap_region_t* prev = NULL;
    mmap_region_t* curr = p->mmap;

    // --- 寻找插入位置（按地址排序）---
    while (curr && curr->begin < begin) {
        prev = curr;
        curr = curr->next;
    }

    // 将 new_node 插入 prev 和 curr 之间
    // 可能是头结点或者尾节点，或是中间节点
    if (prev) {
        prev->next = new_node;
    } else { // 插入作为头结点
        p->mmap = new_node;
    }
    new_node->next = curr;

    // --- 尝试与后继节点(curr)合并 ---
    if (curr && (new_node->begin + new_node->npages * PGSIZE == curr->begin)) {
        // 我们需要保存 curr->next 在合并前，因为合并后 curr 会被释放
        mmap_region_t* next_of_curr = curr->next;
        mmap_merge(new_node, curr, true);
        // 在合并后 new_node 被保留，curr 被释放。
        // 我们必须更新 new_node->next 以跳过已释放的 curr
        new_node->next = next_of_curr;
    }

    // --- 尝试与前驱节点(prev)合并 ---
    if (prev && (prev->begin + prev->npages * PGSIZE == new_node->begin)) {
        // 我们需要保存 new_node->next 在合并前，因为合并后 new_node 会被释放
        mmap_region_t* next_of_new = new_node->next;
        mmap_merge(prev, new_node, true);
        // 在合并后 prev 被保留，new_node 被释放。
        // 我们必须更新 prev->next 以跳过已释放的 new_node
        prev->next = next_of_new;
    }

    // 不需要单独的双向合并逻辑，因为上面的两次合并已经涵盖了所有情况
}

// 用户堆空间增加, 返回新的堆顶地址 (注意栈顶最大值限制)
// 在这里无需修正 p->heap_top
uint64 uvm_heap_grow(pgtbl_t pgtbl, uint64 heap_top, uint32 len)
{
    uint64 new_heap_top = heap_top + len;
    uint64 va;

    if (new_heap_top > TRAPFRAME - PGSIZE) // 栈顶限制
        return 0;

    uint64 start_alloc = ALIGN_UP(heap_top, PGSIZE);
    uint64 end_alloc = ALIGN_UP(new_heap_top, PGSIZE);

    for (va = start_alloc; va < end_alloc; va += PGSIZE) {
        void *pa = pmem_alloc(false);
        if (pa == NULL) {
            uvm_heap_ungrow(pgtbl, va, va - start_alloc);
            return 0;
        }
        memset(pa, 0, PGSIZE);
        if (vm_mappages(pgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W | PTE_U) != 0) {
            pmem_free((uint64)pa, false);
            uvm_heap_ungrow(pgtbl, va, va - start_alloc);
            return 0;
        }
    }

    return new_heap_top;
}

// 用户堆空间减少, 返回新的堆顶地址
// 在这里无需修正 p->heap_top
uint64 uvm_heap_ungrow(pgtbl_t pgtbl, uint64 heap_top, uint32 len)
{
    uint64 new_heap_top = heap_top - len;
    
    uint64 start_free = ALIGN_UP(new_heap_top, PGSIZE);
    uint64 end_free = ALIGN_UP(heap_top, PGSIZE);

    if (end_free > start_free) {
        vm_unmappages(pgtbl, start_free, end_free - start_free, true);
    }

    return new_heap_top;
}

// 用户态地址空间[src, src+len) 拷贝至 内核态地址空间[dst, dst+len)
// 注意: src dst 不一定是 page-aligned
void uvm_copyin(pgtbl_t pgtbl, uint64 dst, uint64 src, uint32 len)
{
    uint64 n, va0, pa0;
    pte_t *pte;

    while(len > 0){
        va0 = ALIGN_DOWN(src, PGSIZE);
        pte = vm_getpte(pgtbl, va0, false);
        if(pte == NULL || (*pte & PTE_V) == 0 || (*pte & PTE_U) == 0)
            panic("uvm_copyin: check pte fail");
        
        pa0 = PTE_TO_PA(*pte);
        n = PGSIZE - (src - va0);
        if(n > len)
            n = len;
        
        memmove((void*)dst, (void*)(pa0 + (src - va0)), n);

        len -= n;
        dst += n;
        src += n;
    }
}

// 内核态地址空间[src, src+len） 拷贝至 用户态地址空间[dst, dst+len)
void uvm_copyout(pgtbl_t pgtbl, uint64 dst, uint64 src, uint32 len)
{
    uint64 n, va0, pa0;
    pte_t *pte;

    while(len > 0){
        va0 = ALIGN_DOWN(dst, PGSIZE);
        pte = vm_getpte(pgtbl, va0, false);
        if(pte == NULL || (*pte & PTE_V) == 0 || (*pte & PTE_U) == 0)
            panic("uvm_copyout: check pte fail");
        
        pa0 = PTE_TO_PA(*pte);
        n = PGSIZE - (dst - va0);
        if(n > len)
            n = len;
        
        memmove((void*)(pa0 + (dst - va0)), (void*)src, n);

        len -= n;
        dst += n;
        src += n;
    }
}

// 用户态字符串拷贝到内核态
// 最多拷贝maxlen字节, 中途遇到'\0'则终止
// 注意: src dst 不一定是 page-aligned
void uvm_copyin_str(pgtbl_t pgtbl, uint64 dst, uint64 src, uint32 maxlen)
{
    uint64 n, va0, pa0;
    int got_null = 0;
    pte_t *pte;

    while(got_null == 0 && maxlen > 0){
        va0 = ALIGN_DOWN(src, PGSIZE);
        pte = vm_getpte(pgtbl, va0, false);
        if(pte == NULL || (*pte & PTE_V) == 0 || (*pte & PTE_U) == 0)
            panic("uvm_copyin_str: check pte fail");
        
        pa0 = PTE_TO_PA(*pte);
        n = PGSIZE - (src - va0);
        if(n > maxlen)
            n = maxlen;
        
        char *p = (char *)(pa0 + (src - va0));
        char *q = (char *)dst;
        while(n > 0){
            if(*p == '\0'){
                *q = '\0';
                got_null = 1;
                break;
            } else {
                *q = *p;
            }
            n--;
            maxlen--;
            p++;
            q++;
            dst++;
            src++;
        }
    }
}