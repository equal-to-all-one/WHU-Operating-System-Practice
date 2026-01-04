# Chapter0 前言

## 0.1 项目整体概述

本项目是基于武汉大学操作系统课程实践要求，参考 xv6-riscv 实现的一个简易操作系统内核。项目旨在从零开始构建一个基于 RISC-V 64位架构的操作系统，涵盖了从裸机启动到文件系统实现的完整全栈开发。

相较于原始的 xv6 实现，本项目在目录结构上进行了模块化重构，使得代码层次更加清晰。项目主要包含以下核心组件与功能：

*   **系统启动 (Boot)**: 实现了多核启动流程，从 M-mode 切换至 S-mode，并完成了 UART 驱动与自旋锁的初始化。
*   **内存管理 (Memory)**: 实现了物理内存分配器（PMM）与基于 Sv39 的三级页表虚拟内存管理（VMM），支持内核空间与用户空间的隔离。
*   **中断与异常 (Trap)**: 构建了完善的中断处理机制，包括 M-mode 时钟中断的委托处理、PLIC 外部中断响应以及系统调用分发。
*   **进程管理 (Process)**: 实现了进程的创建（Fork）、执行（Exec）、退出（Exit）与等待（Wait），支持基于时间片的抢占式调度算法。
*   **用户态虚存 (User VM)**: 支持 `mmap`/`munmap` 内存映射机制以及用户堆（Heap）的动态伸缩。
*   **文件系统 (File System)**: 实现了基于 VirtIO 块设备的完整文件系统，包含缓冲区缓存（Buffer Cache）、Inode 管理、目录操作以及路径解析。

## 0.2 开发环境构建指南

为了确保开发工具链（GCC, Binutils, QEMU）版本的一致性，避免因环境差异导致的编译错误，本项目采用 Docker 容器作为统一的开发与运行环境。

### 0.2.1 构建 Docker 镜像

项目根目录下提供了 `Dockerfile`（通常位于项目根目录或 `devcontainer/` 目录）。为了实现“无上下文构建”，即避免将整个项目目录作为构建上下文发送给 Docker daemon，我们可以显式指定 `Dockerfile` 路径并使用 `/dev/null` 作为构建上下文。这在项目目录较大时可以加快构建速度。

请在项目根目录执行以下命令构建镜像：

```bash
# 构建镜像，标签为 os-practice:qemu-7.2
# -f 指定 Dockerfile 路径。请根据您的实际文件位置调整。
# /dev/null 作为构建上下文，实现无上下文构建。
# 假设 Dockerfile 位于 devcontainer/Dockerfile
docker build -f devcontainer/Dockerfile -t os-practice:qemu-7.2 /dev/null
```

> **注意**：
>
> 1.  如果您的 `Dockerfile` 位于项目根目录，命令应修改为：
>     `docker build -f Dockerfile -t os-practice:qemu-7.2 /dev/null`
> 2.  如果您的 `Dockerfile` 位于其他路径，请相应调整 `-f` 参数。

### 0.2.2 启动实验容器

由于 VS Code 的调试任务依赖于特定的容器名称，**请务必使用以下命令启动容器**，并将容器命名为 `oslab`。同时，我们需要暴露 GDB 调试端口（26000），并将本地项目目录挂载到容器内的 `/riscv-os` 路径，以便进行源码映射。

您提供的命令如下，它以交互模式 (`-it`) 启动容器，并在容器退出时自动删除 (`--rm`)，这非常适合开发环境：

```bash
# 首先进入到项目目录
cd path/to/project
# 启动容器，以交互模式运行，自动删除，映射端口，并命名为 oslab
# --mount type=bind,src=.,dst=/riscv-os 将当前目录挂载到容器内的 /riscv-os 路径，与 launch.json 中的 sourceFileMap 匹配
docker container run -it --name oslab --rm -p 127.0.0.1:26000:26000 --mount type=bind,src=.,dst=/riscv-os os-practice:qemu-7.2 bash
```

> **命令解释**：
>
> *   `docker container run`: 启动一个新的容器。
> *   `-it`: 保持标准输入打开 (`-i`) 并分配一个伪 TTY (`-t`)。这使得容器可以接收键盘输入并显示输出，对于运行 `bash` 或交互式程序（如 QEMU）至关重要。
> *   `--name oslab`: 为容器指定名称为 `oslab`，方便后续通过名称引用。
> *   `--rm`: 当容器退出时，自动删除容器。这有助于保持 Docker 环境的整洁。
> *   `-p 127.0.0.1:26000:26000`: 将本地主机的 `127.0.0.1:26000` 端口映射到容器的 `26000` 端口。这使得本地的 GDB 客户端可以连接到容器内 QEMU 暴露的 GDB stub。
> *   `--mount type=bind,src=.,dst=/riscv-os`: 将当前主机目录（`src=.`）以绑定挂载 (`type=bind`) 的方式挂载到容器内的 `/riscv-os` 目录（`dst=/riscv-os`）。这允许您在主机上编辑代码，并在容器内编译和运行，同时保持源码同步。
> *   `os-practice:qemu-7.2`: 要使用的 Docker 镜像名称和标签。
> *   `bash`: 在容器启动后执行的命令，这里是启动一个 `bash` shell。

> **注意**：启动后，所有的编译和运行命令都将在该容器内部执行。您需要在一个新的终端窗口中执行 `docker exec -it oslab bash` 来进入该容器的 shell，或者直接在启动容器的终端中操作。通过vscode的container Tools更加方便。

## 0.3 VS Code 调试配置详解

本项目推荐使用 VS Code 配合 WSL (Windows Subsystem for Linux) 或 Linux 本地环境进行开发，并通过 GDB 远程连接 Docker 容器进行调试。这种方式虽然配置稍显复杂，但能提供最佳的代码补全与图形化调试体验。

### 0.3.1 调试环境依赖

1. **本地/WSL 环境**：需要安装 `gdb-multiarch`。

   ```bash
   sudo apt update && sudo apt install gdb-multiarch
   ```

2. **VS Code 插件**：安装 `C/C++` 插件。

### 0.3.2 配置文件解析

项目在 `.vscode` 目录下提供了预设的调试配置。

**1. 任务配置 (`tasks.json`)**

该文件定义了在调试开始前如何启动 QEMU。我们使用 `docker exec` 在容器内执行 `make qemu-gdb`。`command` 中的 `-it` 参数对于后续测试 `scanf`/`uart` 输入至关重要，它确保了 Docker 容器的标准输入输出是交互式的。`problemMatcher` 中的 `endsPattern` 字段用于检测 QEMU GDB stub 是否已成功启动并监听端口，从而触发 GDB 连接。

```json
{
    "version": "2.0.0",
    "tasks": [
        {
            "label": "xv6build",
            "type": "shell",
            "isBackground": true,
            // 关键点：使用 -it 参数支持交互式输入，指定容器名为 oslab
            "command": "docker exec -it oslab make qemu-gdb",
            "problemMatcher": [
                {
                    "owner": "xv6",
                    "pattern": [
                        {
                            "regexp": ".",
                            "file": 1,
                            "location": 2,
                            "message": 3
                        }
                    ],
                    // 监控 QEMU 输出，当出现 tcp::26000 时认为启动成功，开始连接 GDB
                    "background": {
                        "beginsPattern": ".*",
                        "endsPattern": "tcp::26000"
                    }
                }
            ]
        }
    ]
}
```

**2. 启动配置 (`launch.json`)**

该文件定义了 GDB 如何连接到 QEMU 以及如何映射源码路径。
`miDebuggerPath` 指向本地或 WSL 中安装的 `gdb-multiarch`。`miDebuggerServerAddress` 连接到 Docker 容器暴露的 GDB 端口。
**`sourceFileMap`** 是远程调试的关键：它将 Docker 容器内部的源码路径 (`/riscv-os`) 映射到本地 VS Code 工作区的路径 (`${workspaceFolder}`)。这样，当 GDB 报告容器内的文件路径时，VS Code 能够找到并显示本地对应的源码文件，实现断点、单步调试等功能。

```json
{
    "version": "0.2.0",
    "configurations": [
        {
            "name": "debug xv6",
            "type": "cppdbg",
            "request": "launch",
            "program": "${workspaceFolder}/kernel-qemu", // 本地可执行文件路径（用于读取符号表）
            "args": [],
            "stopAtEntry": true,
            "cwd": "${workspaceFolder}",
            "miDebuggerServerAddress": "127.0.0.1:26000", // 连接到 Docker 暴露的端口
            "miDebuggerPath": "/usr/bin/gdb-multiarch",   // 本地 GDB 路径
            "environment": [],
            "externalConsole": false,
            "sourceFileMap": {
                // 核心配置：将容器内的源码路径映射到本地
                // "/riscv-os" 是 docker run -v 参数中指定的容器内路径
                "/riscv-os": "${workspaceFolder}"
            },
            "MIMode": "gdb",
            "preLaunchTask": "xv6build", // 调试前先执行上述任务启动 QEMU
            "setupCommands": [
                {
                    "description": "pretty printing",
                    "text": "-enable-pretty-printing",
                    "ignoreFailures": true,
                },
            ],
        }
    ]
}
```

### 0.3.3 调试流程

1.  确保 `oslab` 容器正在运行。如果未运行，请执行 0.2.2 节中的 `docker container run` 命令。
2.  在 VS Code 中打开项目。
3.  点击左侧的 "Run and Debug" 图标（或按 `Ctrl+Shift+D`）。
4.  在顶部下拉菜单中选择 "debug xv6" 配置。
5.  点击绿色的“开始调试”按钮（或按 `F5`）。
6.  VS Code 会自动执行 `xv6build` 任务，在容器内启动 QEMU 并监听 26000 端口。
7.  本地 GDB 连接成功后，即可在源码中设置断点、单步调试、查看变量和寄存器。

> **VS Code Dev Containers 提示**：
> 如果您希望获得更无缝的开发体验，强烈建议使用 VS Code 的 `Remote - Containers` 扩展。通过在项目根目录创建 `.devcontainer` 文件夹并配置 `devcontainer.json` 和 `Dockerfile`，VS Code 可以直接在 Docker 容器内部打开整个工作区。在这种模式下，所有的工具链和路径都天然一致，调试配置会变得极其简单，甚至无需手动配置 `sourceFileMap`。

## 0.4 项目构建与运行命令

以下命令建议在 Docker 容器的终端内执行（可通过 `docker exec -it oslab bash` 进入）：

* **进入容器终端**:

  ```bash
  docker exec -it oslab bash
  ```

* **编译内核**:

  ```bash
  make build
  ```

* **运行 QEMU (无调试)**:

  ```bash
  make qemu
  ```

* **运行 QEMU (带 GDB 支持)**:

  ```bash
  make qemu-gdb
  ```

  > **注意**：此命令会启动 QEMU 并监听 GDB 连接，但不会自动连接 GDB。您可以使用 VS Code 调试器或手动 GDB 命令行进行连接。

* **清理构建产物**:

  ```bash
  make clean
  ```

* **退出 QEMU**: 在 QEMU 窗口中按 `Ctrl+A` 然后按 `X` 键。

## 0.5 避坑指南与开发建议

在完成本项目的过程中，总结了以下常见问题与经验，希望能帮助使用者少走弯路：

1.  **Docker 交互式参数 (`-it`)**:
    在 Chapter 9 中涉及到标准输入（`stdin`）和控制台交互测试。如果 `tasks.json` 或 `docker run` 命令中缺少 `-it` 参数，会导致 `scanf` 或 `uart_getc` 无法接收键盘输入，表现为程序卡死或无回显。

2.  **路径映射 (`sourceFileMap`)**:
    如果调试时断点无法命中，或者 GDB 提示找不到源文件，请务必检查 `launch.json` 中的 `sourceFileMap`。容器内的绝对路径必须与 `docker run -v` 挂载的目标路径严格一致。

3.  **物理内存分配 (`pmem_alloc`)**:
    在使用 `pmem_alloc` 时需格外注意参数。内核页表和内核数据结构应使用 `pmem_alloc(true)`；而用户进程的页表、栈和数据页应使用 `pmem_alloc(false)`。混用会导致 `pmem_free` 时因区域检查失败而触发 Panic。

4.  **锁的粒度与死锁**:
    在文件系统开发中（如 `path_create_inode`），涉及目录锁和 Inode 锁的嵌套获取。务必遵循“先获取父节点锁，获取子节点引用后，尽早释放父节点锁”的原则，并注意错误处理路径上的锁释放，防止死锁。

5.  **GDB 多进程调试**:
    由于 QEMU 模拟的是多核环境，GDB 可能会在不同 CPU 之间切换。调试时建议关注当前断点所在的 CPU 上下文（Hart ID）。
