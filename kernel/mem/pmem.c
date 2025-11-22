#include "mem/pmem.h"
#include "lib/lock.h"
#include "lib/print.h"
#include "lib/str.h"
// 物理页链表节点数据结构
typedef struct page_node {
    struct page_node* next;
} page_node_t;

// 物理页链表
typedef struct alloc_region {
    uint64 begin; // 分配区起始地址
    uint64 end;   // 分配区结束地址
    spinlock_t lock; // 分配区自旋锁(保护链表操作)
    uint32 allocable; // 可分配页数
    page_node_t* list_head; // 物理页链表头节点
} alloc_region_t;

// 内核和用户可分配的物理页分开
static alloc_region_t kern_region, user_region;

#define KERN_PAGES 1024 // 内核可分配空间占1024个pages

// Helper function to initialize a region
static void region_init(alloc_region_t* region, char* name, uint64 begin, uint64 end) {
    spinlock_init(&region->lock, name);
    region->begin = ALIGN_UP(begin, PGSIZE);
    region->end = ALIGN_DOWN(end, PGSIZE);
    region->list_head = NULL;
    region->allocable = 0;

    for (uint64 p = region->begin; p + PGSIZE <= region->end; p += PGSIZE) {
        pmem_free(p, (region == &kern_region));
    }
}

// 物理内存初始化
void  pmem_init(void)
{
    uint64 kern_alloc_end = (uint64) ALLOC_BEGIN + KERN_PAGES * PGSIZE;
    region_init(&kern_region, "kern_region", (uint64)ALLOC_BEGIN, kern_alloc_end);
    region_init(&user_region, "user_region", kern_alloc_end, (uint64)ALLOC_END);
    // region_init(&user_region, "user_region", kern_alloc_end, kern_alloc_end + KERN_PAGES * PGSIZE);
}

// 释放物理页
// 失败则panic锁死
void  pmem_free(uint64 page, bool in_kernel)
{
    // 根据in_kernel选择释放到的区域
    alloc_region_t* region = in_kernel ? &kern_region : &user_region;

    // kalloc.c kfree
    // 检查地址合法性: 4096字节对齐，地址在分配区范围内
    if((page % PGSIZE) != 0 || page >= region->end || (uint64)page < region->begin)
        panic("pmem_free: invalid page address");

    // Fill with junk to catch dangling refs. 填充垃圾数据有助于发现悬空指针引用
    memset((void*)page, 1, PGSIZE);

    page_node_t* p = (page_node_t*)page;

    spinlock_acquire(&region->lock); // 获取内存分配锁
    p->next = region->list_head;
    region->list_head = p; // 将该页加入空闲链表
    region->allocable++;
    spinlock_release(&region->lock); // 释放内存分配锁
}

// 返回一个可分配的干净物理页
// 失败则panic锁死
void* pmem_alloc(bool in_kernel)
{
    alloc_region_t* region = in_kernel ? &kern_region : &user_region;
    
    spinlock_acquire(&region->lock); // 获取内存分配锁
    page_node_t* p = region->list_head; // 从空闲链表中取出第一个空闲页
    if(p) {// 如果有空闲页
        region->list_head = p->next; // 更新空闲链表头指针，指向下一个空闲页
        region->allocable--;
    }
    spinlock_release(&region->lock); // 释放内存分配锁
    if(p) // 如果分配成功
        memset((void*)p, 0, PGSIZE); // fill with junk
    else
        panic("pmem_alloc: out of memory");

    return (void*)p;
}