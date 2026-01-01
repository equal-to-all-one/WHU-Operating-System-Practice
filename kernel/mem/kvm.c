// kernel virtual memory management

#include "mem/pmem.h"
#include "mem/vmem.h"
#include "lib/print.h"
#include "lib/str.h"
#include "riscv.h"
#include "memlayout.h"

extern char trampoline[]; // in trampoline.S

static pgtbl_t kernel_pgtbl; // 内核页表


// 根据pagetable,找到va对应的pte
// 若设置alloc=true 则在PTE无效时尝试申请一个物理页
// 成功返回PTE, 失败返回NULL
// 提示：使用 VA_TO_VPN PTE_TO_PA PA_TO_PTE
pte_t* vm_getpte(pgtbl_t pgtbl, uint64 va, bool alloc)
{
    if(pgtbl == NULL) // 用于文件系统的virtio.c
        pgtbl = kernel_pgtbl;

    // Sv39 虚拟地址必须小于 VA_MAX 
    if (va >= VA_MAX)
        panic("vm_getpte: va >= VA_MAX");
    
    // 逐级遍历三级页表
    for (int level = 2; level > 0; level--) {
        pte_t *pte = &pgtbl[VA_TO_VPN(va, level)];

        if (*pte & PTE_V) {
            // PTE有效，获取下一级页表的物理地址
            pgtbl = (pgtbl_t)PTE_TO_PA(*pte);
        } else {
            // PTE无效，需要分配新的页表页
            if (!alloc)
                return NULL; // 如果不允许分配，则返回失败
            
            pgtbl = (pgtbl_t)pmem_alloc(true); // 为内核分配一个物理页
            if (pgtbl == NULL)
                return NULL; // 分配失败
            
            memset(pgtbl, 0, PGSIZE); // 清零新分配的页
            // 更新当前PTE，使其指向新的下一级页表
            // 中间页表项的权限位(X,W,R)为0，只有V位有效
            *pte = PA_TO_PTE(pgtbl) | PTE_V;
        }
    }
    // 返回最低一级（level-0）页表中对应va的PTE地址
    return &pgtbl[VA_TO_VPN(va, 0)];
}

// 在pgtbl中建立 [va, va + len) -> [pa, pa + len) 的映射
// 本质是找到va在页表对应位置的pte并修改它
// 检查: va pa 应当是 page-aligned, len(字节数) > 0, va + len <= VA_MAX
// 注意: perm 应该如何使用
int32 vm_mappages(pgtbl_t pgtbl, uint64 va, uint64 pa, uint64 len, int perm)
{
    uint64 cur_va, last_va;
    pte_t* pte;

    if ((va % PGSIZE) !=0)
        panic("vm_mappages: va is not page aligned");
    if ((pa % PGSIZE) !=0)
        panic("vm_mappages: pa is not page aligned");
    if (len == 0)
        panic("vm_mappages: len is zero");

    cur_va = va;
    last_va = ALIGN_DOWN(va + len - 1, PGSIZE);

    for (;;) {
        pte = vm_getpte(pgtbl, cur_va, true);
        if (pte == NULL)
            panic("vm_mappages: vm_getpte failed");
        
        // 检查PTE是否已经被映射，如果是，则判断是否是要修改权限位
        if ((*pte & PTE_V) && (PTE_TO_PA(*pte) != pa))
            panic("vm_mappages: remap");

        // 设置PTE：物理地址 + 权限位 + 有效位
        *pte = PA_TO_PTE(pa) | perm | PTE_V;

        if (cur_va == last_va)
            break;
        
        cur_va += PGSIZE;
        pa += PGSIZE;
    }

    return 0;
}

// 解除pgtbl中[va, va+len)区域的映射
// 如果freeit == true则释放对应物理页, 默认是用户的物理页
void vm_unmappages(pgtbl_t pgtbl, uint64 va, uint64 len, bool freeit)
{
    uint64 cur_va, last_va;
    pte_t* pte;

    if ((va % PGSIZE) !=0)
        panic("vm_unmappages: va is not page aligned");
    if (len == 0)
        panic("vm_unmappages: len is zero");
    
    cur_va = va;
    last_va = ALIGN_DOWN(va + len - 1, PGSIZE);

    for (;;) {
        pte = vm_getpte(pgtbl, cur_va, false);
        if (pte != NULL && (*pte & PTE_V)) {
            if (freeit) {
                uint64 pa = PTE_TO_PA(*pte);
                pmem_free(pa, false); // 释放用户物理页
            }
            *pte = 0; // 将PTE置为无效
        }

        if (cur_va == last_va)
            break;
        
        cur_va += PGSIZE;
    }
}


// 填充kernel_pgtbl
// 完成 UART CLINT PLIC 内核代码区 内核数据区 可分配区域 trampoline kstack 的映射
void kvm_init()
{
    // 分配内核根页表
    kernel_pgtbl = (pgtbl_t)pmem_alloc(true);
    if (kernel_pgtbl == NULL)
        panic("kvm_init: pmem_alloc failed");

    memset(kernel_pgtbl, 0, PGSIZE);

    // 映射硬件设备
    vm_mappages(kernel_pgtbl, UART_BASE, UART_BASE, PGSIZE, PTE_R | PTE_W);
    vm_mappages(kernel_pgtbl, VIRTIO_BASE, VIRTIO_BASE, PGSIZE, PTE_R | PTE_W);
    vm_mappages(kernel_pgtbl, CLINT_BASE, CLINT_BASE, CLINT_SIZE, PTE_R | PTE_W);
    vm_mappages(kernel_pgtbl, PLIC_BASE, PLIC_BASE, PLIC_SIZE, PTE_R | PTE_W);

    // 映射内核代码段 (只读+可执行)
    vm_mappages(kernel_pgtbl, KERNEL_BASE, KERNEL_BASE, (uint64)KERNEL_DATA - (uint64)KERNEL_BASE, PTE_R | PTE_X);

    // 映射内核数据段和物理内存分配区 (读+写)
    vm_mappages(kernel_pgtbl, (uint64)KERNEL_DATA, (uint64)KERNEL_DATA, (uint64)ALLOC_END - (uint64)KERNEL_DATA, PTE_R | PTE_W);

    // 映射 trampoline
    vm_mappages(kernel_pgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

    // 映射 kstack
    for(int i = 0; i < NPROC; i++) {
        void *kstack_pa = pmem_alloc(true);
        if(kstack_pa == NULL) panic("kvm_init: kstack alloc failed");
        vm_mappages(kernel_pgtbl, KSTACK(i), (uint64)kstack_pa, PGSIZE, PTE_R | PTE_W);
    }
}

// 使用新的页表，刷新TLB
void kvm_inithart()
{
  // wait for any previous writes to the page table memory to finish.
  sfence_vma();

  w_satp(MAKE_SATP(kernel_pgtbl));

  // flush stale entries from the TLB.
  sfence_vma();
}

// for debug
// 输出页表内容
void vm_print(pgtbl_t pgtbl)
{
    // 顶级页表，次级页表，低级页表
    pgtbl_t pgtbl_2 = pgtbl, pgtbl_1 = NULL, pgtbl_0 = NULL;
    pte_t pte;

    printf("level-2 pgtbl: pa = %p\n", pgtbl_2);
    for(int i = 0; i < PGSIZE / sizeof(pte_t); i++) 
    {
        pte = pgtbl_2[i];
        if(!((pte) & PTE_V)) continue;
        assert(PTE_CHECK(pte), "vm_print: pte check fail (1)");
        pgtbl_1 = (pgtbl_t)PTE_TO_PA(pte);
        printf(".. level-1 pgtbl %d: pa = %p\n", i, pgtbl_1);
        
        for(int j = 0; j < PGSIZE / sizeof(pte_t); j++)
        {
            pte = pgtbl_1[j];
            if(!((pte) & PTE_V)) continue;
            assert(PTE_CHECK(pte), "vm_print: pte check fail (2)");
            pgtbl_0 = (pgtbl_t)PTE_TO_PA(pte);
            printf(".. .. level-0 pgtbl %d: pa = %p\n", j, pgtbl_2);

            for(int k = 0; k < PGSIZE / sizeof(pte_t); k++) 
            {
                pte = pgtbl_0[k];
                if(!((pte) & PTE_V)) continue;
                assert(!PTE_CHECK(pte), "vm_print: pte check fail (3)");
                printf(".. .. .. physical page %d: pa = %p flags = %d\n", k, (uint64)PTE_TO_PA(pte), (int)PTE_FLAGS(pte));                
            }
        }
    }
}