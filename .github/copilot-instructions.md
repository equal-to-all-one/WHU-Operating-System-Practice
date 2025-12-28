# GitHub Copilot Instructions for ECNU-OSLAB

This repository contains a RISC-V Operating System kernel for the ECNU OS Lab.
**Current Focus: Lab 6 - Process Management (Scheduling, Fork/Exit/Wait)**

## 🧠 Project Architecture & Context

- **Core Architecture**: RISC-V 64-bit (`riscv64`).
- **Boot Flow**:
  1. `kernel/boot/entry.S`: `_entry` (M-mode entry point, sets up stack).
  2. `kernel/boot/start.c`: `start()` (Configures M-mode, delegates traps, switches to S-mode).
  3. `kernel/boot/main.c`: `main()` (Kernel initialization, starts scheduler).
- **Process Management** (`kernel/proc/`):
  - `proc.c`: Process lifecycle (`fork`, `exit`, `wait`), scheduler (`proc_scheduler`), and state transitions.
  - `swtch.S`: Context switching assembly (`swtch`).
  - `cpu.c`: CPU-local storage management (`mycpu`, `myproc`).
  - `include/proc/proc.h`: `struct proc`, `struct context`, `struct trapframe`, and `enum proc_state`.
- **System Calls** (`kernel/syscall/`):
  - `syscall.c`: Dispatcher (`syscall()`) and argument retrieval (`arg_raw`, `arg_int`).
  - `sysfunc.c`: Implementation of syscalls (`sys_fork`, `sys_exit`, etc.).
  - `sysnum.h`: System call numbers.
- **Memory Management** (`kernel/mem/`):
  - `pmem.c`: Physical memory allocator.
  - `kvm.c`: Kernel page tables.
  - `uvm.c`: User page tables and data copying (`uvm_copyin`, `uvm_copyout`).
  - `mmap.c`: Memory mapping support.

## 🛠 Development Workflow

### Build & Run
- **Build Kernel**: `make build` (Recursive make system).
- **Run in QEMU**: `make qemu` (Starts QEMU with `kernel-qemu`).
- **Debug**: `make qemu-gdb` (Starts QEMU halted on port 26000).
  - *VS Code Task*: `xv6build` runs `docker exec oslab make qemu-gdb`.

### Key Commands
- Clean: `make clean`

## 📝 Coding Conventions

- **Headers**: All header files are in `include/`. Use relative paths like `#include "lib/print.h"`.
- **Printing**: Use `printf()` from `lib/print.h` for kernel logging.
- **Concurrency**:
  - Use `spinlock_t` (`lib/spinlock.c`) for shared data.
  - `wait_lock` (in `proc.c`) protects parent/child relationships and wait/exit logic.
  - `lk_pid` protects global PID allocation.
  - Always acquire `p->lk` before changing `p->state`.
- **Current Context**:
  - `mycpu()`: Returns pointer to current `struct cpu`.
  - `myproc()`: Returns pointer to current `struct proc` (interrupt safe).
- **User/Kernel Data**:
  - Never dereference user pointers directly. Use `uvm_copyin`/`uvm_copyout` or `arg_*` helpers.

## ⚠️ Common Pitfalls & Patterns

- **Process State Transitions**:
  - Valid transitions: `UNUSED` -> `RUNNABLE` (alloc), `RUNNABLE` <-> `RUNNING` (sched), `RUNNING` -> `SLEEPING` (sleep), `RUNNING`/`RUNNABLE` -> `ZOMBIE` (exit).
  - Must hold `p->lk` when modifying state.
- **Context Switching**:
  - `swtch` saves callee-saved registers (`s0`-`s11`, `ra`, `sp`).
  - `scheduler()` runs on the per-CPU scheduler stack, not a process kernel stack.
- **Trap Handling**:
  - `trap_user.c`: Handles traps from user mode (syscalls, page faults).
  - `trap_kernel.c`: Handles traps from kernel mode.
  - `trampoline.S`: Code mapped at the same high address in user and kernel page tables for trap entry/exit.
- **Address Spaces**:
  - Kernel runs in high memory (virtual).
  - User processes have their own page tables (`p->pagetable`).

## 📂 Key File Locations

- **Process Struct**: `include/proc/proc.h`
- **Scheduler**: `kernel/proc/proc.c`
- **Syscall Dispatch**: `kernel/syscall/syscall.c`
- **Trap Vector**: `kernel/trap/trap.S` & `trampoline.S`
