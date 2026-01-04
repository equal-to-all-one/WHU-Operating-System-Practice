#include "fs/fs.h"
#include "fs/buf.h"
#include "fs/dir.h"
#include "fs/bitmap.h"
#include "fs/inode.h"
#include "fs/file.h"
#include "mem/vmem.h"
#include "proc/cpu.h"
#include "lib/print.h"
#include "dev/console.h"

// 设备列表(读写接口)
dev_t devlist[N_DEV];

// ftable + 保护它的锁
#define N_FILE 32
file_t ftable[N_FILE];
spinlock_t lk_ftable;

// ftable初始化 + devlist初始化
void file_init()
{
    spinlock_init(&lk_ftable, "ftable");
    console_init();
}

// alloc file_t in ftable
// 失败则panic
file_t* file_alloc()
{
    spinlock_acquire(&lk_ftable);
    for(int i = 0; i < N_FILE; i++){
        if(ftable[i].ref == 0){
            ftable[i].ref = 1;
            spinlock_release(&lk_ftable);
            return &ftable[i];
        }
    }
    spinlock_release(&lk_ftable);
    return NULL;
}

// 创建设备文件(供proczero创建console)
file_t* file_create_dev(char* path, uint16 major, uint16 minor)
{
    inode_t* ip = path_create_inode(path, FT_DEVICE, major, minor);
    if(ip == NULL)
        return NULL;
    
    file_t* f = file_alloc();
    if(f == NULL){
        inode_free(ip);
        return NULL;
    }
    
    f->type = FD_DEVICE;
    f->readable = true;
    f->writable = true;
    f->ip = ip;
    f->major = major;
    
    return f;
}

// 打开一个文件
file_t* file_open(char* path, uint32 open_mode)
{
    inode_t* ip;
    file_t* f;

    if(open_mode & MODE_CREATE){
        ip = path_create_inode(path, FT_FILE, 0, 0);
        if(ip == NULL) return NULL;
    } else {
        ip = path_to_inode(path);
        if(ip == NULL) return NULL;
        inode_lock(ip);
        if(ip->type == FT_DIR && (open_mode != MODE_READ)){
            inode_unlock_free(ip);
            return NULL;
        }
        inode_unlock(ip);
    }

    f = file_alloc();
    if(f == NULL){
        inode_free(ip);
        return NULL;
    }

    if(ip->type == FT_DEVICE){
        f->type = FD_DEVICE;
        f->major = ip->major;
    } else if(ip->type == FT_DIR){
        f->type = FD_DIR;
    } else {
        f->type = FD_FILE;
    }

    f->readable = (open_mode & MODE_READ) ? true : false;
    f->writable = (open_mode & MODE_WRITE) ? true : false;
    f->ip = ip;
    f->offset = 0;

    return f;
}

// 释放一个file
void file_close(file_t* file)
{
    spinlock_acquire(&lk_ftable);
    if(file->ref < 1)
        panic("file_close");
    file->ref--;
    if(file->ref > 0){
        spinlock_release(&lk_ftable);
        return;
    }
    
    file_t f = *file;
    file->ref = 0;
    file->type = FD_UNUSED;
    spinlock_release(&lk_ftable);

    if(f.type == FD_FILE || f.type == FD_DIR || f.type == FD_DEVICE){
        inode_free(f.ip);
    }
}

// 文件内容读取
// 返回读取到的字节数
uint32 file_read(file_t* file, uint32 len, uint64 dst, bool user)
{
    if(file->readable == false) return 0;

    if(file->type == FD_DEVICE){
        if(file->major < 0 || file->major >= N_DEV || !devlist[file->major].read)
            return 0;
        return devlist[file->major].read(len, dst, user);
    } else if(file->type == FD_FILE || file->type == FD_DIR){
        inode_lock(file->ip);
        uint32 n = inode_read_data(file->ip, file->offset, len, (void*)dst, user);
        file->offset += n;
        inode_unlock(file->ip);
        return n;
    }
    return 0;
}

// 文件内容写入
// 返回写入的字节数
uint32 file_write(file_t* file, uint32 len, uint64 src, bool user)
{
    if(file->writable == false) return 0;

    if(file->type == FD_DEVICE){
        if(file->major < 0 || file->major >= N_DEV || !devlist[file->major].write)
            return 0;
        return devlist[file->major].write(len, src, user);
    } else if(file->type == FD_FILE){
        inode_lock(file->ip);
        uint32 n = inode_write_data(file->ip, file->offset, len, (void*)src, user);
        file->offset += n;
        inode_unlock(file->ip);
        return n;
    }
    return 0;
}

// flags 可能取值
#define LSEEK_SET 0  // file->offset = offset
#define LSEEK_ADD 1  // file->offset += offset
#define LSEEK_SUB 2  // file->offset -= offset

// 修改file->offset (只针对FD_FILE类型的文件)
uint32 file_lseek(file_t* file, uint32 offset, int flags)
{
    if(file->type != FD_FILE) return -1;

    inode_lock(file->ip);
    switch(flags){
        case LSEEK_SET:
            file->offset = offset;
            break;
        case LSEEK_ADD:
            file->offset += offset;
            break;
        case LSEEK_SUB:
            file->offset -= offset;
            break;
    }
    inode_unlock(file->ip);
    return file->offset;
}

// file->ref++ with lock
file_t* file_dup(file_t* file)
{
    spinlock_acquire(&lk_ftable);
    assert(file->ref > 0, "file_dup: ref");
    file->ref++;
    spinlock_release(&lk_ftable);
    return file;
}

// 获取文件状态
int file_stat(file_t* file, uint64 addr)
{
    file_state_t state;
    if(file->type == FD_FILE || file->type == FD_DIR)
    {
        inode_lock(file->ip);
        state.type = file->ip->type;
        state.inode_num = file->ip->inode_num;
        state.nlink = file->ip->nlink;
        state.size = file->ip->size;
        inode_unlock(file->ip);

        uvm_copyout(myproc()->pgtbl, addr, (uint64)&state, sizeof(file_state_t));
    }
    return -1;
}