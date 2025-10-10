# 实验练习总结

## 练习1：`entry.S`关键指令解析
### `la sp, bootstacktop`
- **操作**：`la`（Load Address）是 RISC - V 汇编的伪指令，功能是将`bootstacktop`符号对应的地址加载到栈指针寄存器`sp`中。
- **目的**：内核启动初期尚未建立进程管理机制，而进入 C 语言环境（执行`kern_init`）前需要栈空间支持函数调用等操作。`bootstack`是在`.data`段定义的一块大小为`KSTACKSIZE`的内存区域，`bootstacktop`指向该栈空间的顶部（RISC - V 架构中栈通常向下生长），这条指令为内核自身初始化了初始栈。

### `tail kern_init`
- **操作**：`tail`是 RISC - V 架构用于函数调用优化的指令，会跳转到`kern_init`函数执行，同时将返回地址设置为特殊值（一般为 - 1），表明这是一个无需返回的调用。
- **目的**：完成栈初始化后，通过该指令将控制权从汇编代码传递给 C 语言实现的内核初始化函数`kern_init`。使用`tail`而非普通的`jal`（跳转并链接）指令，可节省一个指令周期，因为内核入口函数不需要返回。这一操作标志着内核启动过程从汇编阶段进入 C 语言阶段，后续页表设置、中断控制器初始化等初始化工作都在`kern_init`中完成。


## 练习2：RISC - V 内核启动流程调试
### 实验环境
- **硬件模拟**：Qemu（模拟 RISC - V 64 位硬件平台）；
- **调试工具**：GDB（`riscv64 - unknown - elf - gdb`）；
- **启动固件**：OpenSBI v0.4；
- **内核代码**：ucore lab1 内核代码。

### 调试过程与分析
#### （一）调试环境搭建与初始化
1. **启动 Qemu 并等待 GDB 连接**：执行`make debug`命令，Qemu 因`-S`（暂停 CPU）和`-s`（开启 1234 端口）参数，进入等待调试器连接状态。同时，OpenSBI 正常启动并输出平台信息（如`Platform Name: QEMU Virt Machine`等），说明 bootloader 阶段初始化正常。
2. **启动 GDB 并连接 Qemu**：执行`make gdb`命令，GDB 自动加载内核符号文件（`bin/kernel`），并通过`target remote localhost:1234`连接到 Qemu 的 1234 端口，成功建立远程调试会话。
最初执行 make debug 时，终端提示 qemu-system-riscv64: -s: Failed to find an available port: Address already in use，这表明 QEMU 尝试使用的调试端口（默认是 1234）已被占用。之后使用sudo netstat -tulnp | grep :1234指令找到该端口并通过
最初执行 make debug 时，终端提示 qemu-system-riscv64: -s: Failed to find an available port: Address already in use，这表明 QEMU 尝试使用的调试端口（默认是 1234）已被占用。之后使用sudo netstat -tulnp | grep :1234指令找到该端口并通过sudo kill -9 3594解放进程后重新进行调试
#### （二）跟踪加电后初始执行流程
RISC - V 处理器加电后，首先执行 Qemu 内置的 MROM 代码，初始执行地址为`0x1000`。通过 GDB 的`x/10i 0x1000`命令查看该地址附近的指令，可知此处代码主要完成最基础的硬件初始化（如内存控制器初始化），并最终跳转到 OpenSBI 的入口地址`0x80000000`。

#### （三）跟踪 OpenSBI 加载内核流程
OpenSBI 启动后，负责将内核镜像加载到指定内存地址`0x80200000`。为验证这一过程，在 GDB 中执行`watch *0x80200000`命令设置内存观察点。当执行`c`（继续）命令时，GDB 会在 OpenSBI 向`0x80200000`地址写入内核数据时暂停，再通过`x/16x 0x80200000`查看该地址内存内容，可确认内核镜像已被成功加载。

#### （四）跟踪内核入口与初始化函数执行
1. **内核入口（汇编阶段）**：在 GDB 中设置断点`b *0x80200000`（内核加载地址，对应`kern/init/entry.S`的`kern_entry`函数入口），继续执行后，GDB 会停在`kern_entry`的第一条指令`la sp, bootstacktop`处。该指令的作用是将内核栈的顶部地址`bootstacktop`加载到`sp`寄存器（栈指针），为后续 C 语言函数调用（如`kern_init`）准备栈空间。
2. **跳转到 C 语言初始化函数**：执行`si`（单步指令），可跟踪到`tail kern_init`指令。`tail`是 RISC - V 的跳转指令，此指令将 CPU 控制权从汇编代码移交到 C 语言编写的`kern_init`函数（位于`kern/init/init.c`）。
3. **`kern_init`函数执行**：进入`kern_init`后，能看到`memset(edata, 0, end - edata);`等初始化代码执行。该代码的作用是将内核的未初始化数据段（`bss`段）清零，确保全局变量等在初始化时拥有正确的初始值。