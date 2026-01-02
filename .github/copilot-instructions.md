# GitHub Copilot Instructions for ECNU-OSLAB

This repository contains a RISC-V Operating System kernel for the ECNU OS Lab.
**Current Focus: Lab 8 - File System High-Level Abstraction (Inodes, Directories, Path Resolution)**

## 🧠 Project Architecture & Context

- **Core Architecture**: RISC-V 64-bit (`riscv64`).
- **Boot Flow**:
  1. `kernel/boot/entry.S`: `_entry` (M-mode entry point).
  2. `kernel/boot/start.c`: `start()` (Configures M-mode, switches to S-mode).
  3. `kernel/boot/main.c`: `main()` (Kernel initialization, starts scheduler).
  4. **FS Init**: `fs_init()` is called in `kernel/proc/proc.c:fork_return()` (first process context) because it requires sleeping.
- **File System Stack** (Bottom-Up):
  1.  **Device Driver** (`kernel/dev/virtio.c`): VirtIO block device driver.
  2.  **Buffer Cache** (`kernel/fs/buf.c`): Caches disk blocks (`buf_t`).
  3.  **Bitmap** (`kernel/fs/bitmap.c`): Manages free/used disk blocks.
  4.  **Inode Layer** (`kernel/fs/inode.c`): Manages file metadata (`inode_t`). Handles mapping file offsets to disk blocks.
  5.  **Directory Layer** (`kernel/fs/dir.c`): Manages directory entries (`dirent_t`) and hierarchy.
  6.  **Path Resolution** (`kernel/fs/dir.c`): Resolves paths to inodes (`path_to_inode`).
- **Process Management** (`kernel/proc/`): `proc.c`, `swtch.S`, `cpu.c`.
- **Memory Management** (`kernel/mem/`): `pmem.c`, `kvm.c`, `uvm.c`.

## 🛠 Development Workflow

### Build & Run
- **Build All**: `make build` (Builds kernel, user programs, and `mkfs`).
- **Run QEMU**: `make qemu` (Runs kernel with `fs.img` attached).
- **Debug**: `make qemu-gdb` (Starts QEMU halted on port 26000).
  - *VS Code Task*: `xv6build` runs `docker exec oslab make qemu-gdb`.
- **File System Image**: `mkfs/mkfs.c` compiles to `mkfs/mkfs`, which generates `fs.img`.

### Key Commands
- Clean: `make clean`

## 📝 Coding Conventions

- **Headers**: Use relative paths like `#include "fs/inode.h"`.
- **Printing**: `printf()` from `lib/print.h`.
- **Concurrency**:
  - **Spinlocks**: `spinlock_t` (`lib/spinlock.c`) for short critical sections (e.g., `lk_icache`).
  - **Sleeplocks**: `sleeplock_t` (`lib/sleeplock.c`) for long operations (e.g., disk I/O). `inode_t` uses `slk`.
- **Inodes**:
  - **Disk vs Memory**: `inode_disk_t` (64 bytes on disk) vs `inode_t` (in-memory with lock & ref count).
  - **Syncing**: `inode_rw(ip, write)` syncs between memory and disk.
  - **Locking**: `inode_lock(ip)` / `inode_unlock(ip)` uses sleeplocks.
  - **Ref Counting**: `ip->ref` tracks in-memory pointers. `ip->nlink` tracks directory entries on disk.
- **Directories**:
  - **Entry**: `dirent_t` (32 bytes: `uint16 inode_num`, `char name[30]`).
  - **Operations**: `dir_search_entry`, `dir_add_entry`, `dir_delete_entry`.
  - **Root**: `INODE_ROOT` is 0.
- **Types**:
  - `inode_num` is `uint16`.
  - `INODE_NUM_UNUSED` is `0xFFFF`.

## ⚠️ Common Pitfalls & Patterns

- **Inode Locking**:
  - Always hold `ip->slk` when reading/writing inode content or data.
  - `inode_get` increments ref count but doesn't lock. `inode_lock` locks it.
  - `inode_put` releases ref count (and sleeps if it needs to free the inode).
- **Deadlocks**:
  - Watch out for lock ordering when traversing directories (e.g., `..`).
- **Buffer Cache Interaction**:
  - Inode functions use `buf_read` to access disk blocks. Remember to `buf_release`.
- **TODOs**:
  - Implement directory operations in `kernel/fs/dir.c`: `dir_search_entry`, `dir_add_entry`, `dir_delete_entry`.
  - Implement path resolution: `path_to_inode`, `path_to_pinode`.
  - Implement `dir_get_entries` and `dir_change`.

## 📂 Key File Locations

- **Inodes**: `kernel/fs/inode.c`, `include/fs/inode.h`
- **Directories**: `kernel/fs/dir.c`, `include/fs/dir.h`
- **File System Main**: `kernel/fs/fs.c`, `include/fs/fs.h`
- **Buffer Cache**: `kernel/fs/buf.c`, `include/fs/buf.h`
- **Disk Driver**: `kernel/dev/virtio.c`
