# Copilot Instructions (ECNU-OSLAB / Lab 9)

This repository contains a small RISC-V (rv64) teaching OS kernel with a userland and a simple file system.

## Architecture Overview

- **Kernel**: `kernel/`
  - `boot/`: Entry point (`entry.S`), initialization (`main.c`).
  - `proc/`: Process management (`proc.c`), context switching (`swtch.S`), execution (`exec.c`).
  - `mem/`: Physical (`pmem.c`) and virtual (`uvm.c`, `kvm.c`) memory, mmap (`mmap.c`).
  - `fs/`: File system implementation (inodes, buffers, directory, file).
  - `syscall/`: System call dispatch (`syscall.c`) and implementation (`sysproc.c`, `sysfile.c`).
  - `trap/`: Trap handling (`trap.S`, `trap_kernel.c`, `trap_user.c`).
  - `dev/`: Device drivers (console, uart, virtio, plic, timer).
- **Userland**: `user/` (programs and library).
- **Headers**: `include/` (mirrors kernel structure).
- **Tools**: `mkfs/` (file system image creator).

## Build & Run

- **Build**: `make build` (builds kernel, user programs, and `fs.img`).
- **Run QEMU**: `make qemu`.
- **Debug**: `make qemu-gdb` (starts QEMU with GDB stub on port 26000).
- **Clean**: `make clean`.

## Key Workflows

### Adding a User Program
1.  Create `user/foo.c`.
2.  Add `_foo` to `UPROGS` in `user/Makefile`.
3.  Run `make build` to compile and pack it into `fs.img`.

### Adding a System Call
1.  **Define Number**: Add `SYS_foo` to `user/syscall_num.h` and `include/syscall/sysnum.h`.
2.  **User Stub**: Add function prototype to `user/userlib.h` and implementation to `user/user_syscall.c` (using `syscall(SYS_foo, ...)`).
3.  **Kernel Handler**:
    -   Implement `sys_foo()` in `kernel/syscall/sysproc.c` (process-related) or `kernel/syscall/sysfile.c` (file-related).
    -   Add `[SYS_foo] sys_foo` to `syscalls[]` in `kernel/syscall/syscall.c`.
    -   Add prototype to `include/syscall/sysfunc.h`.
4.  **Argument Handling**: Use `arg_uint64`, `arg_int`, `arg_str` (wraps `uvm_copyin_str`) in `kernel/syscall/syscall.c` to retrieve arguments from trapframe.

### Memory Management
-   **Physical Memory**: `pmem_alloc(bool in_kernel)` and `pmem_free(void *pa, bool in_kernel)`.
    -   `in_kernel = true`: Allocates/frees from kernel pool.
    -   `in_kernel = false`: Allocates/frees from user pool.
-   **Virtual Memory**:
    -   `kvm_init()`: Sets up kernel page table.
    -   `uvm_init()`: Sets up user page table.
    -   `vm_mappages()`: Maps physical to virtual addresses.

### File System
-   **Initialization**: `fs_init()` is called from `fork_return()` in `kernel/proc/proc.c` (context of the first process), *not* `main()`.
-   **Locking**:
    -   Use `inode_lock(ip)` (sleeplock) for inode operations.
    -   Always release buffers with `buf_release(b)` after use.
-   **Path**: `kernel/fs/` contains `inode.c`, `file.c`, `dir.c`, `fs.c` (superblock).

## Project Conventions

-   **Process State**: `proc_t` in `include/proc/proc.h`.
-   **First Process**: Created by `proc_make_first()` in `kernel/proc/proc.c`.
-   **Trap Handling**: `trap_user` handles syscalls and exceptions from user mode; `trap_kernel` handles interrupts/exceptions in kernel mode.
-   **Console**: `file_create_dev("/console", DEV_CONSOLE, 0)` sets up stdin/stdout/stderr for the first process.
