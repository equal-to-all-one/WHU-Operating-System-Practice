include common.mk

KERN = kernel
USER = user
KERNEL_ELF = kernel-qemu
CPUNUM = 2
FS_IMG = none

.PHONY: clean $(KERN) $(USER)

# $(MAKE): 这是一个特殊的 make 变量，它总是指向当前正在执行的 make 程序。
# 使用 $(MAKE) 而不是硬编码的 make 是一个好习惯，
# 因为它可以将命令行选项（如 -j 并行构建）传递给子 make 进程。
$(KERN):
	$(MAKE) build --directory=$@

$(USER):
	$(MAKE) init --directory=$@

# QEMU相关配置
QEMU     =  qemu-system-riscv64
QEMUOPTS =  -machine virt -bios none -kernel $(KERNEL_ELF) 
QEMUOPTS += -m 128M -smp $(CPUNUM) -nographic

# 调试
# GDBPORT = $(shell expr `id -u` % 5000 + 25000)
GDBPORT ?= 26000
QEMUGDB = $(shell if $(QEMU) -help | grep -q '^-gdb'; \
	then echo "-gdb tcp::$(GDBPORT)"; \
	else echo "-s -p $(GDBPORT)"; fi)

build: $(USER) $(KERN)

# qemu运行
qemu: $(USER) $(KERN)
	$(QEMU) $(QEMUOPTS)

.gdbinit: .gdbinit.tmpl-riscv
	sed "s/:1234/:$(GDBPORT)/" < $^ > $@

qemu-gdb: $(USER) $(KERN) .gdbinit
	$(QEMU) $(QEMUOPTS) -S $(QEMUGDB)

clean:
	$(MAKE) --directory=$(KERN) clean
	rm -f $(KERNEL_ELF) .gdbinit