# GitHub Copilot Instructions for ECNU-OSLAB

This repository contains a RISC-V Operating System kernel for the ECNU OS Lab.
**Current Focus: Lab 7 - File System & Device Drivers (VirtIO, Buffer Cache)**

## 🧠 Project Architecture & Context

- **Core Architecture**: RISC-V 64-bit (`riscv64`).
- **Boot Flow**:
  1. `kernel/boot/entry.S`: `_entry` (M-mode entry point).
  2. `kernel/boot/start.c`: `start()` (Configures M-mode, switches to S-mode).
  3. `kernel/boot/main.c`: `main()` (Kernel initialization, starts scheduler).
- **File System Stack** (Bottom-Up):
  1.  **Device Driver** (`kernel/dev/virtio.c`): VirtIO block device driver. Handles disk I/O via MMIO and interrupts.
  2.  **Buffer Cache** (`kernel/fs/buf.c`): Caches disk blocks in memory. Uses `buf_t` with sleeplocks.
  3.  **Bitmap** (`kernel/fs/bitmap.c`): Manages free/used disk blocks.
  4.  **File System** (`kernel/fs/fs.c`): Inodes, directories, and file operations.
- **Process Management** (`kernel/proc/`): `proc.c`, `swtch.S`, `cpu.c`.
- **Memory Management** (`kernel/mem/`): `pmem.c`, `kvm.c` (maps VirtIO MMIO), `uvm.c`.
- **Traps**: `trap_kernel.c` (handles disk interrupts), `trap_user.c`.

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

- **Headers**: Use relative paths like `#include "fs/buf.h"`.
- **Printing**: `printf()` from `lib/print.h`.
- **Concurrency**:
  - **Spinlocks**: `spinlock_t` (`lib/spinlock.c`) for short critical sections (e.g., `lk_buf_cache`).
  - **Sleeplocks**: `sleeplock_t` (`lib/sleeplock.c`) for long operations (e.g., disk I/O). `buf_t` uses `slk`.
  - **Locking Order**: Acquire `lk_buf_cache` to find a buffer, then acquire `b->slk` to use it. Release `lk_buf_cache` after getting `b->slk`.
- **Buffer Cache**:
  - `buf_read(block_num)`: Returns a locked buffer.
  - `buf_write(b)`: Writes buffer to disk.
  - `buf_release(b)`: Releases buffer lock and decrements ref count.
- **VirtIO**:
  - Uses MMIO registers defined in `virtio.h`.
  - `virtio_disk_rw(b, write)`: Performs the actual I/O.

## ⚠️ Common Pitfalls & Patterns

- **Buffer Locking**:
  - A buffer returned by `buf_read` is **locked** (`sleeplock`). You must release it with `buf_release`.
  - Do not hold `spinlock` while sleeping (waiting for disk I/O).
- **Disk Interrupts**:
  - Disk interrupts trigger `virtio_disk_intr()`, which wakes up processes waiting on buffers.
- **Memory Mapping**:
  - `kvm.c` maps the VirtIO MMIO region so the kernel can access device registers.
- **TODOs**:
  - Implement `buf_read`, `buf_write`, `buf_release` in `kernel/fs/buf.c`.
  - Implement file system logic in `kernel/fs/fs.c` and `kernel/fs/bitmap.c`.

## 📂 Key File Locations

- **Buffer Cache**: `kernel/fs/buf.c`, `include/fs/buf.h`
- **Disk Driver**: `kernel/dev/virtio.c`, `include/dev/virtio.h`
- **File System**: `kernel/fs/fs.c`, `include/fs/fs.h`
- **Bitmap**: `kernel/fs/bitmap.c`
- **Make Tool**: `mkfs/mkfs.c`
