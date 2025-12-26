# GitHub Copilot Instructions for ECNU-OSLAB

This repository contains a RISC-V Operating System kernel for the ECNU OS Lab (specifically Lab 3: Interrupts & Exceptions).

## 🧠 Project Architecture & Context

- **Core Architecture**: RISC-V 64-bit (`riscv64`).
- **Boot Flow**:
  1. `kernel/boot/entry.S`: `_entry` (M-mode entry point, sets up stack).
  2. `kernel/boot/start.c`: `start()` (Configures M-mode, delegates traps, switches to S-mode).
  3. `kernel/boot/main.c`: `main()` (Kernel initialization).
- **Memory Management**:
  - `mem/pmem.c`: Physical memory allocator.
  - `mem/kvm.c`: Kernel virtual memory (page tables).
  - `include/memlayout.h`: Physical memory layout definitions.
- **Trap Handling** (Focus of Lab 3):
  - `kernel/trap/trap.S`: Assembly trap vector (saves/restores context).
  - `kernel/trap/trap_kernel.c`: C interrupt/exception handlers.
  - **PLIC**: Platform-Level Interrupt Controller (External interrupts).
  - **CLINT**: Core Local Interruptor (Timer/Software interrupts).

## 🛠 Development Workflow

### Build & Run
- **Build Kernel**: `make build` (Recursive make system).
- **Run in QEMU**: `make qemu` (Starts QEMU with `kernel-qemu`).
- **Debug**: `make qemu-gdb` (Starts QEMU halted, listening on port 26000).
  - *Note*: The workspace has a VS Code task `xv6build` that runs `docker exec oslab make qemu-gdb`.

### Key Commands
- Clean: `make clean`

## 📝 Coding Conventions

- **Headers**: All header files are in `include/`. Use relative paths like `#include "lib/print.h"`.
- **Printing**: Use `printf()` from `lib/print.h` for kernel logging.
- **Types**: Use explicit width types (`uint64`, `uint32`) defined in `include/common.h` or `include/riscv.h`.
- **Synchronization**: Use `spinlock` (from `lib/spinlock.c`) for shared data protection.
- **Hardware Access**: Use helper functions in `include/riscv.h` for CSR (Control and Status Register) access (e.g., `r_tp()`, `w_sie()`).

## ⚠️ Common Pitfalls & Patterns

- **Interrupts vs Exceptions**:
  - **Interrupts**: Async (Timer, UART). Return PC = Next Instruction.
  - **Exceptions**: Sync (Page Fault, Illegal Instruction). Return PC = Current Instruction (usually).
- **Hart ID**: `r_tp()` returns the current Hart (Hardware Thread/CPU) ID.
- **TODOs**: Look for `// TODO` comments in `kernel/dev/timer.c` and `kernel/trap/trap_kernel.c` for lab tasks.
- **Address Translation**: Remember the distinction between Physical Addresses (PA) and Virtual Addresses (VA). The kernel runs in high memory (virtual) mapped to low memory (physical).

## 📂 Key File Locations

- **Entry**: `kernel/boot/entry.S`
- **Main**: `kernel/boot/main.c`
- **Trap Vector**: `kernel/trap/trap.S`
- **Trap Handler**: `kernel/trap/trap_kernel.c`
- **Timer Driver**: `kernel/dev/timer.c`
- **UART Driver**: `kernel/dev/uart.c`
