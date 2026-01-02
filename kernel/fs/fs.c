#include "fs/fs.h"
#include "fs/buf.h"
#include "fs/bitmap.h"
#include "fs/inode.h"
#include "fs/dir.h"
#include "lib/str.h"
#include "lib/print.h"

// 超级块在内存的副本
super_block_t sb;

#define FS_MAGIC 0x12345678
#define SB_BLOCK_NUM 0

static char str[BLOCK_SIZE];
static char tmp[BLOCK_SIZE];
static char empty[BLOCK_SIZE];

static bool blockcmp(void* a, void* b)
{
    char* ca = (char*)a;
    char* cb = (char*)b;
    for(int i = 0; i < BLOCK_SIZE; i++)
        if(ca[i] != cb[i])
            return false;
    return true;
}

// 输出super_block的信息
static void sb_print()
{
    printf("\nsuper block information:\n");
    printf("magic = %x\n", sb.magic);
    printf("block size = %d\n", sb.block_size);
    printf("inode blocks = %d\n", sb.inode_blocks);
    printf("data blocks = %d\n", sb.data_blocks);
    printf("total blocks = %d\n", sb.total_blocks);
    printf("inode bitmap start = %d\n", sb.inode_bitmap_start);
    printf("inode start = %d\n", sb.inode_start);
    printf("data bitmap start = %d\n", sb.data_bitmap_start);
    printf("data start = %d\n", sb.data_start);
}

// 文件系统初始化
void fs_init()
{
    buf_init();

    buf_t* buf; 
    buf = buf_read(SB_BLOCK_NUM);
    memmove(&sb, buf->data, sizeof(sb));
    assert(sb.magic == FS_MAGIC, "fs_init: magic");
    assert(sb.block_size == BLOCK_SIZE, "fs_init: block size");
    buf_release(buf);
    sb_print();

    inode_init(); 

    uint32 ret = 0;

    for(int i = 0; i < BLOCK_SIZE; i++) {
        str[i] = i;
        empty[i] = 0;
    }

    // 创建新的inode
    inode_t* nip = inode_create(FT_FILE, 0, 0);
    inode_lock(nip);
    
    // 第一次查看
    inode_print(nip);
    bitmap_print(sb.data_bitmap_start);

    uint32 max_blocks =  N_ADDRS_1 + N_ADDRS_2 * ENTRY_PER_BLOCK + 2 * ENTRY_PER_BLOCK;

    for(uint32 i = 0; i < max_blocks; i++)
    {
        ret = inode_write_data(nip, i * BLOCK_SIZE, BLOCK_SIZE, str, false);
        assert(ret == BLOCK_SIZE, "inode_write_data fail");
    }
    ret = inode_write_data(nip, (max_blocks - 2) * BLOCK_SIZE, BLOCK_SIZE, empty, false);
    assert(ret == BLOCK_SIZE, "inode_write_data fail");
    
    // 第二次查看
    inode_print(nip);

    // 区域-1
    ret = inode_read_data(nip, BLOCK_SIZE, BLOCK_SIZE, tmp, false);
    assert(ret == BLOCK_SIZE, "inode_read_data fail");
    assert(blockcmp(tmp, str), "check-1 fail");
    printf("check-1 success\n");

    // 区域-2
    ret = inode_read_data(nip, N_ADDRS_1 * BLOCK_SIZE, BLOCK_SIZE, tmp, false);
    assert(ret == BLOCK_SIZE, "inode_read_data fail");
    assert(blockcmp(tmp, str), "check-2 fail");
    printf("check-2 success\n");

    // 区域-3
    ret = inode_read_data(nip, (max_blocks - 2) * BLOCK_SIZE, BLOCK_SIZE, tmp, false);
    assert(ret == BLOCK_SIZE, "inode_read_data fail");
    assert(blockcmp(tmp, empty), "check-2 fail");
    printf("check-3 success\n");

    // 释放inode管理的所有data block
    inode_free_data(nip);
    printf("free success\n");

    // 第三次观察
    inode_print(nip);
    bitmap_print(sb.data_bitmap_start);

    inode_unlock_free(nip);
    
    while (1);
}