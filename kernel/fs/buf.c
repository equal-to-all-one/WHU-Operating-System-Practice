#include "fs/buf.h"
#include "dev/vio.h"
#include "lib/lock.h"
#include "lib/print.h"
#include "lib/str.h"

#define N_BLOCK_BUF 6
#define BLOCK_NUM_UNUSED 0xFFFFFFFF

// 将buf包装成双向循环链表的node
typedef struct buf_node {
    buf_t buf;
    struct buf_node* next;
    struct buf_node* prev;
} buf_node_t;

// buf cache
static buf_node_t buf_cache[N_BLOCK_BUF];
static buf_node_t head_buf; // ->next 已分配 ->prev 可分配
static spinlock_t lk_buf_cache; // 这个锁负责保护 链式结构 + buf_ref + block_num

// 链表操作
static void insert_head(buf_node_t* buf_node, bool head_next)
{
    // 如果节点已经在链表中，则先删除

    // 离开
    if(buf_node->next && buf_node->prev) {
        buf_node->next->prev = buf_node->prev;
        buf_node->prev->next = buf_node->next;
    }

    // 插入
    if(head_next) { // 插入 head->next
        buf_node->prev = &head_buf;
        buf_node->next = head_buf.next;
        head_buf.next->prev = buf_node;
        head_buf.next = buf_node;        
    } else { // 插入 head->prev
        buf_node->next = &head_buf;
        buf_node->prev = head_buf.prev;
        head_buf.prev->next = buf_node;
        head_buf.prev = buf_node;
    }
}

// 初始化
void buf_init()
{
    spinlock_init(&lk_buf_cache, "buf_cache");
    head_buf.next = &head_buf;
    head_buf.prev = &head_buf;

    for(int i = 0; i < N_BLOCK_BUF; i++) {
        buf_node_t* b = &buf_cache[i];
        sleeplock_init(&b->buf.slk, "buf_slk");
        b->buf.block_num = BLOCK_NUM_UNUSED;
        b->buf.buf_ref = 0;
        b->buf.dirty = false;
        b->buf.disk = false;
        // 初始时全部插入到 head.prev (空闲链表)
        insert_head(b, false);
    }
}

/*
    首先假设这个block_num对应的block在内存中有备份, 找到它并上锁返回
    如果找不到, 尝试申请一个无人使用的buf, 去磁盘读取对应block并上锁返回
    如果没有空闲buf, panic报错
    (建议合并xv6的bget())
*/
buf_t* buf_read(uint32 block_num)
{
    buf_node_t* b_node;
    buf_t* b;

    spinlock_acquire(&lk_buf_cache);

    // 1. 查找是否已缓存
    for(b_node = head_buf.next; b_node != &head_buf; b_node = b_node->next) {
        if(b_node->buf.block_num == block_num) {
            b_node->buf.buf_ref++;
            insert_head(b_node, true); // 移到 MRU
            spinlock_release(&lk_buf_cache);
            sleeplock_acquire(&b_node->buf.slk);
            return &b_node->buf;
        }
    }

    // 2. 未缓存，寻找 victim (LRU: 从 head.next 开始找第一个 ref=0)
    // 注意：head.next 是 Active 端，head.prev 是 Free 端
    // 遍历顺序：Active -> ... -> Oldest Free -> ... -> Newest Free
    // 所以找到的第一个 ref=0 就是 Oldest Free
    for(b_node = head_buf.next; b_node != &head_buf; b_node = b_node->next) {
        if(b_node->buf.buf_ref == 0) {
            b = &b_node->buf;
            b->buf_ref = 1; // 预定
            uint32 old_block_num = b->block_num;
            b->block_num = BLOCK_NUM_UNUSED; // 暂时移除
            spinlock_release(&lk_buf_cache);

            sleeplock_acquire(&b->slk);

            // 写回脏数据
            if(b->dirty && old_block_num != BLOCK_NUM_UNUSED) {
                b->block_num = old_block_num; // 恢复 block_num 以便写入正确扇区
                virtio_disk_rw(b, 1); // write
                b->dirty = false;
            }

            // 读取新数据
            b->block_num = block_num;
            virtio_disk_rw(b, 0); // read

            // 重新加锁检查 race condition
            spinlock_acquire(&lk_buf_cache);
            buf_node_t* other_node;
            for(other_node = head_buf.next; other_node != &head_buf; other_node = other_node->next) {
                if(other_node->buf.block_num == block_num && other_node != b_node) {
                    // 竞争失败：别人已经加载了这个 block
                    sleeplock_release(&b->slk);
                    b->buf_ref = 0;
                    b->block_num = BLOCK_NUM_UNUSED;
                    insert_head(b_node, false); // 放回空闲链表
                    
                    other_node->buf.buf_ref++;
                    spinlock_release(&lk_buf_cache);
                    sleeplock_acquire(&other_node->buf.slk);
                    return &other_node->buf;
                }
            }
            
            // 竞争成功
            insert_head(b_node, true); // 移到 MRU
            spinlock_release(&lk_buf_cache);
            return b;
        }
    }

    panic("buf_read: no free buf");
    return NULL;
}

// 写函数 (强制磁盘和内存保持一致)
void buf_write(buf_t* buf)
{
    if(buf == NULL) return;
    // Lazy write: 只标记 dirty
    // 实际写入在 buf_read 驱逐时发生
    buf->dirty = true;
}

// buf 释放
void buf_release(buf_t* buf)
{
    if(buf == NULL) return;

    sleeplock_release(&buf->slk);

    spinlock_acquire(&lk_buf_cache);
    buf->buf_ref--;
    if(buf->buf_ref == 0) {
        // 移到 head.prev (Newest Free)
        buf_node_t* node = (buf_node_t*)buf; // buf_node_t 和 buf_t 地址相同
        insert_head(node, false);
    }
    spinlock_release(&lk_buf_cache);
}

// 输出buf_cache的情况
void buf_print()
{
    printf("\nbuf_cache:\n");
    buf_node_t* buf = head_buf.next;
    spinlock_acquire(&lk_buf_cache);
    while(buf != &head_buf)
    {
        buf_t* b = &buf->buf;
        printf("buf %d: ref = %d, block_num = %d\n", (int)(buf-buf_cache), b->buf_ref, b->block_num);
        for(int i = 0; i < 8; i++)
            printf("%d ",b->data[i]);
        printf("\n");
        buf = buf->next;
    }
    spinlock_release(&lk_buf_cache);
}