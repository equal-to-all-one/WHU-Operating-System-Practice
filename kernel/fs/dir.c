#include "fs/fs.h"
#include "fs/buf.h"
#include "fs/inode.h"
#include "fs/dir.h"
#include "fs/bitmap.h"
#include "lib/str.h"
#include "lib/print.h"
#include "proc/cpu.h"
#include "mem/vmem.h"

// 对目录文件的简化性假设: 每个目录文件只包括一个block
// 也就是每个目录下最多 BLOCK_SIZE / sizeof(dirent_t) = 32 个目录项

// 查询一个目录项是否在目录里
// 成功返回这个目录项的inode_num
// 失败返回INODE_NUM_UNUSED
// ps: 调用者需持有pip的锁
uint16 dir_search_entry(inode_t *pip, char *name)
{
    dirent_t de;

    if(pip->type != FT_DIR)
        panic("dirlookup not DIR");

    for(uint32 off = 0; off < pip->size; off += sizeof(de)){
        if(inode_read_data(pip, off, sizeof(de), &de, false) != sizeof(de))
            panic("dir_search_entry: read");
        if(de.inode_num == INODE_NUM_UNUSED)
            continue;
        if(strncmp(name, de.name, DIR_NAME_LEN) == 0)
            return de.inode_num;
    }
    return INODE_NUM_UNUSED;
}

// 在pip目录下添加一个目录项
// 成功返回这个目录项的偏移量 (同时更新pip->size)
// 失败返回BLOCK_SIZE (没有空间 或 发生重名)
// ps: 调用者需持有pip的锁
uint32 dir_add_entry(inode_t *pip, uint16 inode_num, char *name)
{
    uint32 off;
    dirent_t de;
    uint32 empty_off = BLOCK_SIZE;

    for(off = 0; off < pip->size; off += sizeof(de)){
        if(inode_read_data(pip, off, sizeof(de), &de, false) != sizeof(de))
            panic("dir_add_entry: read");
        if(de.inode_num == INODE_NUM_UNUSED){ // 记录第一个空闲目录项位置
            if(empty_off == BLOCK_SIZE)
                empty_off = off;
        } else { // 比较重名
            if(strncmp(name, de.name, DIR_NAME_LEN) == 0)
                return BLOCK_SIZE;
        }
    }

    de.inode_num = inode_num;
    strncpy(de.name, name, DIR_NAME_LEN);

    if(empty_off != BLOCK_SIZE){ // 找到了空闲目录项位置
        if(inode_write_data(pip, empty_off, sizeof(de), &de, false) != sizeof(de))
            panic("dir_add_entry: write");
        return empty_off;
    }

    if(pip->size + sizeof(de) > BLOCK_SIZE) // 超过32个条目
        return BLOCK_SIZE;

    //仍有空间，追加到末尾
    off = pip->size; 
    // write_data中会更新pip->size
    if(inode_write_data(pip, off, sizeof(de), &de, false) != sizeof(de))
        panic("dir_add_entry: write append");
    
    // 再次更新元数据，保持健壮性，write_data也更新了，所以这里也可以删去
    inode_rw(pip, true);
    return off;
}

// 在pip目录下删除一个目录项
// 成功返回这个目录项的inode_num
// 失败返回INODE_NUM_UNUSED
// ps: 调用者需持有pip的锁
uint16 dir_delete_entry(inode_t *pip, char *name)
{
    dirent_t de;
    for(uint32 off = 0; off < pip->size; off += sizeof(de)){
        if(inode_read_data(pip, off, sizeof(de), &de, false) != sizeof(de))
            panic("dir_delete_entry: read");
        if(de.inode_num == INODE_NUM_UNUSED)
            continue;
        if(strncmp(name, de.name, DIR_NAME_LEN) == 0){
            uint16 inum = de.inode_num;
            de.inode_num = INODE_NUM_UNUSED;
            memset(de.name, 0, DIR_NAME_LEN);
            if(inode_write_data(pip, off, sizeof(de), &de, false) != sizeof(de))
                panic("dir_delete_entry: write");
            return inum;
        }
    }
    return INODE_NUM_UNUSED;
}

// 把目录下的有效目录项复制到dst (dst区域长度为len)
// 返回读到的字节数 (sizeof(dirent_t)*n)
// 调用者需要持有pip的锁
uint32 dir_get_entries(inode_t* pip, uint32 len, void* dst, bool user)
{
    dirent_t de;
    uint32 count = 0;
    uint32 off;
    char* d = (char*)dst;

    for(off = 0; off < pip->size; off += sizeof(de)){
        if(inode_read_data(pip, off, sizeof(de), &de, false) != sizeof(de))
            panic("dir_get_entries: read");
        if(de.inode_num != INODE_NUM_UNUSED){
            if(count + sizeof(de) > len)
                break;
            if(user){
                uvm_copyout(myproc()->pgtbl, (uint64)d, (uint64)&de, sizeof(de));
            } else {
                memmove(d, &de, sizeof(de));
            }
            d += sizeof(de);
            count += sizeof(de);
        }
    }
    return count;
}

// 改变进程里存储的当前目录
// 成功返回0 失败返回-1
uint32 dir_change(char* path)
{
    inode_t *ip;
    proc_t *p = myproc();

    ip = path_to_inode(path);
    if(ip == NULL)
        return -1;
    
    inode_lock(ip);
    if(ip->type != FT_DIR){
        inode_unlock_free(ip);
        return -1;
    }
    inode_unlock(ip);

    inode_t *old = p->cwd;
    p->cwd = ip;
    if(old)
        inode_free(old);
    
    return 0;
}

// 输出一个目录下的所有有效目录项
// for debug
// ps: 调用者需持有pip的锁
void dir_print(inode_t *pip)
{
    assert(sleeplock_holding(&pip->slk), "dir_print: lock");

    printf("\ninode_num = %d dirents:\n", pip->inode_num);

    dirent_t *de;
    buf_t *buf = buf_read(pip->addrs[0]);
    for (uint32 offset = 0; offset < BLOCK_SIZE; offset += sizeof(dirent_t))
    {
        de = (dirent_t *)(buf->data + offset);
        if (de->name[0] != 0)
            printf("inum = %d dirent = %s\n", de->inode_num, de->name);
    }
    buf_release(buf);
}

/*----------------------- 路径(一串目录和文件) -------------------------*/

// Examples:
//   skipelem("a/bb/c", name) = "bb/c", setting name = "a"
//   skipelem("///a//bb", name) = "bb", setting name = "a"
//   skipelem("a", name) = "", setting name = "a"
//   skipelem("", name) = skipelem("////", name) = 0
static char *skip_element(char *path, char *name)
{
    while(*path == '/') path++;
    if(*path == 0) return 0;

    char *s = path;
    while (*path != '/' && *path != 0)
        path++;

    int len = path - s;
    if (len >= DIR_NAME_LEN) {
        memmove(name, s, DIR_NAME_LEN);
    } else {
        memmove(name, s, len);
        name[len] = 0;
    }
    while (*path == '/')
        path++;
    return path;
}

// 查找路径path对应的inode (find_parent = false)
// 查找路径path对应的inode的父节点 (find_parent = true)
// 供两个上层函数使用
// 失败返回NULL
static inode_t* search_inode(char* path, char* name, bool find_parent)
{
    inode_t *ip, *next;
    char buf[DIR_NAME_LEN];

    if(*path == '/') {
        ip = inode_alloc(INODE_ROOT);
    } else {
        ip = inode_dup(myproc()->cwd);
    }

    while((path = skip_element(path, buf)) != 0){
        inode_lock(ip);
        if(ip->type != FT_DIR){
            inode_unlock_free(ip);
            return NULL;
        }
        
        if(find_parent && *path == '\0'){
            // Stop one level early
            inode_unlock(ip);
            strncpy(name, buf, DIR_NAME_LEN);
            return ip;
        }

        uint16 inum = dir_search_entry(ip, buf);
        if(inum == INODE_NUM_UNUSED){
            inode_unlock_free(ip);
            return NULL;
        }

        next = inode_alloc(inum);
        inode_unlock(ip);
        inode_free(ip);
        ip = next;
    }
    
    if(find_parent){
        inode_free(ip);
        return NULL;
    }
    
    return ip;
}

// 找到path对应的inode
inode_t* path_to_inode(char* path)
{
    char name[DIR_NAME_LEN];
    return search_inode(path, name, false);
}

// 找到path对应的inode的父节点
// path最后的目录名放入name指向的空间
inode_t* path_to_pinode(char* path, char* name)
{
    return search_inode(path, name, true);
}

// 如果path对应的inode存在则返回引用但是未上锁的inode
// 如果path对应的inode不存在则创建inode（引用但是未上锁）
// 失败返回NULL
inode_t* path_create_inode(char* path, uint16 type, uint16 major, uint16 minor)
{
    char name[DIR_NAME_LEN];
    inode_t *pip, *ip;

    pip = path_to_pinode(path, name);
    if(pip == NULL)
        return NULL;
    
    inode_lock(pip);
    
    uint16 inum = dir_search_entry(pip, name);
    if(inum != INODE_NUM_UNUSED){
        ip = inode_alloc(inum);
        inode_unlock_free(pip);
        return ip;
    }

    ip = inode_create(type, major, minor);
    if(ip == NULL){
        inode_unlock_free(pip);
        return NULL;
    }

    if(dir_add_entry(pip, ip->inode_num, name) == BLOCK_SIZE){
        inode_unlock_free(pip);
        // 这里是没有竞争的，因为刚创建的inode没有被其他进程引用
        inode_lock(ip);
        ip->nlink = 0;
        inode_rw(ip, true);
        inode_unlock_free(ip);
        
        return NULL;
    }

    if(type == FT_DIR){
        inode_lock(ip);
        dir_add_entry(ip, ip->inode_num, ".");
        dir_add_entry(ip, pip->inode_num, "..");
        ip->nlink++;
        inode_rw(ip, true);
        inode_unlock(ip);

        pip->nlink++;
        inode_rw(pip, true);
    }

    inode_unlock_free(pip);
    return ip;
}

// 文件链接(目录不能被链接)
// 本质是创建一个目录项, 这个目录项的inode_num是存在的而不用申请
// 成功返回0 失败返回-1
uint32 path_link(char* old_path, char* new_path)
{
    char name[DIR_NAME_LEN];
    inode_t *ip, *pip;

    ip = path_to_inode(old_path);
    if(ip == NULL)
        return -1;
    
    inode_lock(ip);
    if(ip->type == FT_DIR){
        inode_unlock_free(ip);
        return -1;
    }
    ip->nlink++;
    inode_rw(ip, true);
    inode_unlock(ip);

    pip = path_to_pinode(new_path, name);
    if(pip == NULL){
        inode_lock(ip);
        ip->nlink--;
        inode_rw(ip, true);
        inode_unlock_free(ip);
        return -1;
    }

    inode_lock(pip);
    if(dir_search_entry(pip, name) != INODE_NUM_UNUSED || 
       dir_add_entry(pip, ip->inode_num, name) == BLOCK_SIZE){
        inode_unlock_free(pip);
        inode_lock(ip);
        ip->nlink--;
        inode_rw(ip, true);
        inode_unlock_free(ip);
        return -1;
    }
    
    inode_unlock_free(pip);
    inode_free(ip);
    return 0;
}

// 检查一个unlink操作是否合理
// 调用者需要持有ip的锁
// 在path_unlink()中调用
static bool check_unlink(inode_t* ip)
{
    assert(sleeplock_holding(&ip->slk), "check_unlink: slk");

    // 三个目录项，其中两个是"."和".."，所以是为了看是否还有其他目录项存在
    uint8 tmp[sizeof(dirent_t) * 3];
    uint32 read_len;
    
    read_len = dir_get_entries(ip, sizeof(dirent_t) * 3, tmp, false);
    
    // 一个目录下只能有"."和".."两个目录项才能被删除
    if(read_len == sizeof(dirent_t) * 3) {
        return false;
    } else if(read_len == sizeof(dirent_t) * 2) {
        return true;
    } else {
        panic("check_unlink: read_len");
        return false;
    }
}

// 文件删除链接
uint32 path_unlink(char* path)
{
    char name[DIR_NAME_LEN];
    inode_t *pip, *ip;
    uint16 inum;

    pip = path_to_pinode(path, name);
    if(pip == NULL)
        return -1;
    
    inode_lock(pip);
    
    if(strncmp(name, ".", DIR_NAME_LEN) == 0 || strncmp(name, "..", DIR_NAME_LEN) == 0){
        inode_unlock_free(pip);
        return -1;
    }

    inum = dir_search_entry(pip, name);
    if(inum == INODE_NUM_UNUSED){
        inode_unlock_free(pip);
        return -1;
    }

    ip = inode_alloc(inum);
    inode_lock(ip);

    if(ip->type == FT_DIR && !check_unlink(ip)){
        inode_unlock_free(ip);
        inode_unlock_free(pip);
        return -1;
    }

    inode_unlock(ip);

    if(dir_delete_entry(pip, name) == INODE_NUM_UNUSED){
        inode_free(ip);
        inode_unlock_free(pip);
        return -1;
    }
    inode_unlock_free(pip);

    inode_lock(ip);
    if(ip->nlink < 1)
        panic("path_unlink: nlink < 1");
    ip->nlink--;
    inode_rw(ip, true);
    inode_unlock_free(ip);

    return 0;
}