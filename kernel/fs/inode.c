#include "fs/buf.h"
#include "fs/bitmap.h"
#include "fs/inode.h"
#include "fs/fs.h"
#include "mem/vmem.h"
#include "proc/cpu.h"
#include "lib/print.h"
#include "lib/str.h"
#include "fs/inode.h"

extern super_block_t sb;

// 内存中的inode资源 + 保护它的锁
#define N_INODE 32
static inode_t icache[N_INODE];
static spinlock_t lk_icache;



// icache初始化
void inode_init()
{
    spinlock_init(&lk_icache, "icache");
    for(int i = 0; i < N_INODE; i++) {
        sleeplock_init(&icache[i].slk, "inode");
    }
}

/*---------------------- 与inode本身相关 -------------------*/

// 使用磁盘里的inode更新内存里的inode (write = false)
// 或 使用内存里的inode更新磁盘里的inode (write = true)
// 调用者需要设置inode_num并持有睡眠锁
void inode_rw(inode_t* ip, bool write)
{
    buf_t* buf;
    inode_disk_t* dip;
    uint32 block_num;

    block_num = INODE_LOCATE_BLOCK(ip->inode_num, sb);
    buf = buf_read(block_num);
    dip = (inode_disk_t*)(buf->data) + (ip->inode_num % INODE_PER_BLOCK);

    if(write) {
        dip->type = ip->type;
        dip->major = ip->major;
        dip->minor = ip->minor;
        dip->nlink = ip->nlink;
        dip->size = ip->size;
        memmove(dip->addrs, ip->addrs, sizeof(ip->addrs));
        buf_write(buf);
    } else {
        ip->type = dip->type;
        ip->major = dip->major;
        ip->minor = dip->minor;
        ip->nlink = dip->nlink;
        ip->size = dip->size;
        memmove(ip->addrs, dip->addrs, sizeof(ip->addrs));

        // 如果读取到的 inode 类型为 0 (通常表示空闲或无效 inode)，则触发 panic
        if (ip->type == 0)
            panic("ilock: no type");
    }
    buf_release(buf);
}

// 在icache里查询inode
// 如果没有查询到则申请一个空闲inode
// 如果icache没有空闲inode则报错
// 注意: 获得的inode没有上锁
inode_t* inode_alloc(uint16 inode_num)
{    
    inode_t* ip;

    spinlock_acquire(&lk_icache);
    for(ip = &icache[0]; ip < &icache[N_INODE]; ip++) {
        if(ip->ref > 0 && ip->inode_num == inode_num) {
            ip->ref++;
            spinlock_release(&lk_icache);
            return ip;
        }
    }

    for(ip = &icache[0]; ip < &icache[N_INODE]; ip++) {
        if(ip->ref == 0) {
            ip->ref = 1;
            ip->inode_num = inode_num;
            ip->valid = false;
            spinlock_release(&lk_icache);
            return ip;
        }
    }
    spinlock_release(&lk_icache);
    panic("inode_alloc: no free inode");
    return 0;
}

// 在磁盘里申请一个inode (操作bitmap, 返回inode_num)
// 向icache申请一个inode数据结构
// 填写内存里的inode并以此更新磁盘里的inode
// 注意: 获得的inode没有上锁
inode_t* inode_create(uint16 type, uint16 major, uint16 minor)
{
    uint16 inode_num = bitmap_alloc_inode();
    inode_t* ip = inode_alloc(inode_num);
    
    sleeplock_acquire(&ip->slk);
    ip->type = type;
    ip->major = major;
    ip->minor = minor;
    ip->nlink = 1;
    ip->size = 0;
    memset(ip->addrs, 0, sizeof(ip->addrs));
    
    inode_rw(ip, true); // Write to disk
    sleeplock_release(&ip->slk);
    
    return ip;
}

// 供inode_free调用
// 在磁盘上删除一个inode及其管理的文件 (修改inode bitmap + block bitmap)
// 调用者需要持有lk_icache, 但不应该持有slk
static void inode_destroy(inode_t* ip)
{
    // 安全性说明：
    // 虽然我们在持有自旋锁(lk_icache)的情况下获取睡眠锁(slk)，
    // 但由于 ip->ref == 1，保证了没有其他进程持有该 inode 的引用，
    // 因此也没有其他进程能持有该 inode 的睡眠锁。
    // 所以这里的 sleeplock_acquire 不会睡眠，是安全的。
    // 这样做消除了“释放自旋锁”和“获取睡眠锁”之间的竞态窗口。
    sleeplock_acquire(&ip->slk);
    spinlock_release(&lk_icache);

    inode_free_data(ip);
    bitmap_free_inode(ip->inode_num);
    ip->type = 0;
    ip->valid = false;
    inode_rw(ip, true);

    sleeplock_release(&ip->slk);

    spinlock_acquire(&lk_icache);
}

// 向icache里归还inode
// inode->ref--
// 调用者不应该持有slk,因为我们可能会重新获取这个锁
void inode_free(inode_t* ip)
{
    spinlock_acquire(&lk_icache);
    if(ip->ref == 1 && ip->valid && ip->nlink == 0) {
        // ref == 1 means we are the last reference.
        // nlink == 0 means it should be deleted from disk.
        // We keep ref == 1 to prevent others from allocating this slot while we destroy it.
        inode_destroy(ip);
        ip->ref--;
    } else {
        ip->ref--;
    }
    spinlock_release(&lk_icache);
}

// ip->ref++ with lock
inode_t* inode_dup(inode_t* ip)
{
    spinlock_acquire(&lk_icache);
    ip->ref++;
    spinlock_release(&lk_icache);
    return ip;
}

// 给inode上锁
// 如果valid失效则从磁盘中读入
void inode_lock(inode_t* ip)
{
    sleeplock_acquire(&ip->slk);
    if(!ip->valid) {
        inode_rw(ip, false);
        ip->valid = true;
    }
}

// 给inode解锁
void inode_unlock(inode_t* ip)
{
    sleeplock_release(&ip->slk);
}

// 连招: 解锁 + 释放
void inode_unlock_free(inode_t* ip)
{
    inode_unlock(ip);
    inode_free(ip);
}

/*---------------------------- 与inode管理的data相关 --------------------------*/

// 辅助 inode_locate_block
// 递归查询或创建block
static uint32 locate_block(uint32* entry, uint32 bn, uint32 size)
{
    if(*entry == 0)
        *entry = bitmap_alloc_block();

    if(size == 1)
        return *entry;    

    uint32* next_entry;
    uint32 next_size = size / ENTRY_PER_BLOCK;
    uint32 next_bn = bn % next_size;
    uint32 ret = 0;

    buf_t* buf = buf_read(*entry);
    next_entry = (uint32*)(buf->data) + bn / next_size;
    // ret = locate_block(next_entry, next_bn, next_size);

    // 记录修改前的旧值
    unsigned int old_val = *next_entry; 
    
    ret = locate_block(next_entry, next_bn, next_size);

    // 修复：如果 locate_block 内部修改了 buf 里的值（即分配了新块），必须写回磁盘
    if (*next_entry != old_val) {
        buf_write(buf); 
    }
    buf_release(buf);

    return ret;
}

// 确定inode里第bn块data block的block_num
// 如果不存在第bn块data block则申请一个并返回它的block_num
// 由于inode->addrs的结构, 这个过程比较复杂, 需要单独处理
static uint32 inode_locate_block(inode_t* ip, uint32 bn)
{
    if(bn < N_ADDRS_1)
        return locate_block(&ip->addrs[bn], bn, 1);
    
    bn -= N_ADDRS_1;
    if(bn < N_ADDRS_2 * ENTRY_PER_BLOCK) {
        uint32 idx = bn / ENTRY_PER_BLOCK;
        uint32 off = bn % ENTRY_PER_BLOCK;
        return locate_block(&ip->addrs[N_ADDRS_1 + idx], off, ENTRY_PER_BLOCK);
    }
    
    bn -= N_ADDRS_2 * ENTRY_PER_BLOCK;
    if(bn < N_ADDRS_3 * ENTRY_PER_BLOCK * ENTRY_PER_BLOCK) {
        uint32 idx = bn / (ENTRY_PER_BLOCK * ENTRY_PER_BLOCK);
        uint32 off = bn % (ENTRY_PER_BLOCK * ENTRY_PER_BLOCK);
        return locate_block(&ip->addrs[N_ADDRS_1 + N_ADDRS_2 + idx], off, ENTRY_PER_BLOCK * ENTRY_PER_BLOCK);
    }
    
    panic("inode_locate_block: overflow");
    return 0;
}

#define min(a, b) ((a) < (b) ? (a) : (b))

// 读取 inode 管理的 data block
// 调用者需要持有 inode 锁
// 成功返回读出的字节数, 失败返回0
uint32 inode_read_data(inode_t* ip, uint32 offset, uint32 len, void* dst, bool user)
{
    uint32 tot, m;
    uint32 block_num;
    buf_t* buf;

    if(offset > ip->size || offset + len < offset)
        return 0;
    if(offset + len > ip->size)
        len = ip->size - offset;

    for(tot = 0; tot < len; tot += m, offset += m, dst = (char*)dst + m) {
        block_num = inode_locate_block(ip, offset / BLOCK_SIZE);
        if(block_num == 0)
            break;
        buf = buf_read(block_num);
        m = min(len - tot, BLOCK_SIZE - offset % BLOCK_SIZE);
        if(user) {
            uvm_copyout(myproc()->pgtbl, (uint64)dst, (uint64)(buf->data + offset % BLOCK_SIZE), m);
        } else {
            memmove(dst, buf->data + offset % BLOCK_SIZE, m);
        }
        buf_release(buf);
    }
    return tot;
}

// 写入 inode 管理的 data block (可能导致管理的 block 增加)
// 调用者需要持有 inode 锁
// 成功返回写入的字节数, 失败返回0
uint32 inode_write_data(inode_t* ip, uint32 offset, uint32 len, void* src, bool user)
{
    uint32 tot, m;
    uint32 block_num;
    buf_t* buf;

    if(offset + len < offset)
        return 0;
    if(offset + len > INODE_MAXSIZE)
        return 0;

    for(tot = 0; tot < len; tot += m, offset += m, src = (char*)src + m) {
        block_num = inode_locate_block(ip, offset / BLOCK_SIZE);
        if (block_num == 0)
            break;
        buf = buf_read(block_num);
        m = min(len - tot, BLOCK_SIZE - offset % BLOCK_SIZE);
        if(user) {
            uvm_copyin(myproc()->pgtbl, (uint64)(buf->data + offset % BLOCK_SIZE), (uint64)src, m);
        } else {
            memmove(buf->data + offset % BLOCK_SIZE, src, m);
        }
        buf_write(buf);
        buf_release(buf);
    }

    if(offset > ip->size) {
        ip->size = offset;
        inode_rw(ip, true);
    }
    return tot;
}

// 辅助 inode_free_data 做递归释放
static void data_free(uint32 block_num, uint32 level)
{  
    assert(block_num != 0, "data_free: block_num = 0"); // 不能摧毁根目录

    // block_num 是 data block
    if(level == 0) goto ret;

    // block_num 是 metadata block
    buf_t* buf = buf_read(block_num);
    for(uint32* addr = (uint32*)buf->data; addr < (uint32*)(buf->data + BLOCK_SIZE); addr++) 
    {
        if(*addr == 0) continue;
        data_free(*addr, level - 1);
    }
    buf_release(buf);

ret: // 不管如何都会执行，释放当前的 block_num（matadata 或 data）
    bitmap_free_block(block_num);
    return;
}

// 释放inode管理的 data block
// ip->addrs被清空 ip->size置0
// 调用者需要持有slk
void inode_free_data(inode_t* ip)
{
    for(int i = 0; i < N_ADDRS; i++) {
        if(ip->addrs[i]) {
            if(i < N_ADDRS_1)
                data_free(ip->addrs[i], 0);
            else if(i < N_ADDRS_1 + N_ADDRS_2)
                data_free(ip->addrs[i], 1);
            else
                data_free(ip->addrs[i], 2);
            ip->addrs[i] = 0;
        }
    }
    ip->size = 0;
    inode_rw(ip, true);
}

static char* inode_types[] = {
    "INODE_UNUSED",
    "INODE_DIR",
    "INODE_FILE",
    "INODE_DEVICE",
};

// 输出inode信息
// for dubug
void inode_print(inode_t* ip)
{
    assert(sleeplock_holding(&ip->slk), "inode_print: lk");

    printf("\ninode information:\n");
    printf("num = %d, ref = %d, valid = %d\n", ip->inode_num, ip->ref, ip->valid);
    printf("type = %s, major = %d, minor = %d, nlink = %d\n", inode_types[ip->type], ip->major, ip->minor, ip->nlink);
    printf("size = %d, addrs =", ip->size);
    for(int i = 0; i < N_ADDRS; i++)
        printf(" %d", ip->addrs[i]);
    printf("\n");
}