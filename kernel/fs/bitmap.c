#include "fs/buf.h"
#include "fs/fs.h"
#include "fs/bitmap.h"
#include "lib/print.h"

extern super_block_t sb;

// search and set bit
static uint32 bitmap_search_and_set(uint32 bitmap_block)
{
    buf_t* buf = buf_read(bitmap_block);
    uint32 byte, shift;
    uint8 bit_cmp;

    for(byte = 0; byte < BLOCK_SIZE; byte++) {
        bit_cmp = 1;
        for(shift = 0; shift <= 7; shift++) {
            if((buf->data[byte] & bit_cmp) == 0) {
                buf->data[byte] |= bit_cmp;
                buf_write(buf);
                buf_release(buf);
                return byte * 8 + shift;
            }
            bit_cmp <<= 1;
        }
    }
    buf_release(buf);
    panic("bitmap_search_and_set: no free bit");
    return 0;
}

// unset bit
static void bitmap_unset(uint32 bitmap_block, uint32 num)
{
    buf_t* buf = buf_read(bitmap_block);
    uint32 byte = num / 8;
    uint32 shift = num % 8;
    uint8 bit_cmp = 1 << shift;

    if((buf->data[byte] & bit_cmp) == 0)
        panic("bitmap_unset: bit already free");

    buf->data[byte] &= ~bit_cmp;
    buf_write(buf);
    buf_release(buf);
}

uint32 bitmap_alloc_block()
{
    uint32 offset = bitmap_search_and_set(sb.data_bitmap_start);
    return sb.data_start + offset;
}

void bitmap_free_block(uint32 block_num)
{
    uint32 offset = block_num - sb.data_start;
    bitmap_unset(sb.data_bitmap_start, offset);
}

// 注意编号从1开始而不是0
uint16 bitmap_alloc_inode()
{
    uint32 offset = bitmap_search_and_set(sb.inode_bitmap_start);
    return (uint16)(offset + 1); // inode 0 is reserved
}

void bitmap_free_inode(uint16 inode_num)
{
    uint32 offset = inode_num - 1;
    bitmap_unset(sb.inode_bitmap_start, offset);
}

// 打印所有已经分配出去的bit序号(序号从0开始)
// for debug
void bitmap_print(uint32 bitmap_block_num)
{
    uint8 bit_cmp;
    uint32 byte, shift;

    printf("\nbitmap:\n");

    buf_t* buf = buf_read(bitmap_block_num);
    for(byte = 0; byte < BLOCK_SIZE; byte++) {
        bit_cmp = 1;
        for(shift = 0; shift <= 7; shift++) {
            if(bit_cmp & buf->data[byte])
               printf("bit %d is alloced\n", byte * 8 + shift);
            bit_cmp = bit_cmp << 1;
        }
    }
    printf("over\n");
    buf_release(buf);
}