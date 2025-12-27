#include "lib/print.h"
#include "lib/str.h"
#include "lib/lock.h"
#include "mem/pmem.h"
#include "mem/vmem.h"
#include "mem/mmap.h"

// 包装 mmap_region_t 用于仓库组织
typedef struct mmap_region_node {
    mmap_region_t mmap;
    struct mmap_region_node* next;
} mmap_region_node_t;

// #define N_MMAP 256
#define N_MMAP 64

// mmap_region_node_t 仓库(单向链表) + 指向链表头节点的指针 + 保护仓库的锁
static mmap_region_node_t list_mmap_region_node[N_MMAP];
static mmap_region_node_t* list_head;
static spinlock_t list_lk;

// 初始化上述三个数据结构
void mmap_init()
{
    spinlock_init(&list_lk, "mmap_list");
    
    // list_head points to the first element, which acts as a sentinel/dummy head
    list_head = &list_mmap_region_node[0];
    
    // Link the rest of the nodes
    mmap_region_node_t* current = list_head;
    for (int i = 1; i < N_MMAP; i++) {
        current->next = &list_mmap_region_node[i];
        current = current->next;
    }
    current->next = NULL;
}

// 从仓库申请一个 mmap_region_t
// 若申请失败则 panic
// 注意: list_head 保留, 不会被申请出去
mmap_region_t* mmap_region_alloc()
{
    spinlock_acquire(&list_lk);
    
    if (list_head->next == NULL) {
        spinlock_release(&list_lk);
        panic("mmap_region_alloc: out of memory");
    }
    
    mmap_region_node_t* node = list_head->next;
    list_head->next = node->next;
    
    // Clear the next pointer of the allocated node for safety
    node->next = NULL; 
    
    spinlock_release(&list_lk);
    
    // Initialize the mmap_region_t part
    memset(&node->mmap, 0, sizeof(mmap_region_t));
    
    return &node->mmap;
}

// 向仓库归还一个 mmap_region_t
void mmap_region_free(mmap_region_t* mmap)
{
    if (mmap == NULL) return;
    // 强制类型转换能成立是因为 mmap_region_t 是 mmap_region_node_t 的第一个成员
    // mmap_region_t的地址就是 mmap_region_node_t 的地址
    mmap_region_node_t* node = (mmap_region_node_t*)mmap; 
    
    spinlock_acquire(&list_lk);
    
    node->next = list_head->next;
    list_head->next = node;
    
    spinlock_release(&list_lk);
}

// 输出仓库里可用的 mmap_region_node_t
// for debug
void mmap_show_mmaplist()
{
    spinlock_acquire(&list_lk);
    
    mmap_region_node_t* tmp = list_head;

    int node = 1, index = 0;
    
    while (tmp)
    {
        index = tmp - list_head;
        printf("node %d index = %d\n", node++, index);
        tmp = tmp->next;
    }

    spinlock_release(&list_lk);
}