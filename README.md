# Agent Cell

一个面向 AI Agent 单次任务的实验性 microVM 运行时。

Agent Cell 使用 C11 编写，直接调用 Linux KVM API，在无固件、无磁盘的 Guest 中启动精简 Linux，并执行一个预置的静态工作负载。项目用于验证：Agent 沙盒是否可以通过收缩设备模型和运行时能力，获得比通用 microVM 更小的实现与资源开销。

> **项目状态：Experimental**
>
> 当前版本已经打通 aarch64 KVM 端到端执行链路，但尚未完成生产级隔离加固。请勿直接用于运行来自不可信租户的代码。

## 设计范围

Agent Cell 不是通用虚拟机，也不是容器运行时。当前 Demo 遵循以下约束：

- 一个 VM 只执行一个任务，任务结束后销毁 VM。
- 固定 1 个 vCPU 和 128 MiB Guest RAM。
- Linux kernel 直接启动，不经过 BIOS 或 UEFI。
- 工作负载编译进 initramfs，不挂载磁盘或根文件系统镜像。
- Guest 默认没有网卡、PCI、Shell、SSH、包管理器和后台服务。
- VMM 仅处理启动、控制台、完成通知和异常退出所需的 KVM exit。

如果需求包含 OCI 镜像、通用网络、持久磁盘、快照或成熟的多租户安全能力，应优先选择 Firecracker 等经过生产验证的运行时。Agent Cell 的价值在于研究更窄的 Agent 执行模型，而不是替代完整 microVM 产品。

## 架构

```text
Host
┌──────────────────────────────────────────────────────┐
│ agent-cell                                           │
│                                                      │
│  CLI ──► lifecycle / timeout ──► KVM VM + 1 vCPU     │
│                                      │               │
└──────────────────────────────────────┼───────────────┘
                                       │ direct boot
Guest                                  ▼
┌──────────────────────────────────────────────────────┐
│ minimal Linux Image                                  │
│                                                      │
│  /init (PID 1) ──fork/exec──► /agent-task            │
│         │                              │              │
│         └──── check exit status ◄──────┘              │
│                    │                                 │
│                    └── completion marker + reset     │
└──────────────────────────────────────────────────────┘
```

一次执行只有在下列条件全部满足时才成功：

1. Linux 成功启动 `/init`。
2. `/init` 成功执行 `/agent-task`。
3. `/agent-task` 以状态码 `0` 退出。
4. Guest 通过专用 I/O 通道写入完整完成标记。
5. Guest 发起预期的 reset/shutdown，且全流程未超时。

Guest panic、非零任务状态、错误完成标记、未知 KVM exit 或超时均视为失败。

## 实现状态

状态定义：**已验证**表示完成真实 `/dev/kvm` 端到端运行；**已实现**表示代码已落地，但尚未在当前基准环境重新验证。

| 能力 | aarch64 | x86_64 |
| --- | --- | --- |
| VM/vCPU 创建与 KVM smoke test | 已验证 | 已实现 |
| Linux 无固件直接启动 | 已验证 | 已实现 |
| `/init` 执行静态工作负载 | 已验证 | 已实现 |
| 完成标记、异常退出与超时 | 已验证 | 已实现 |
| 动态任务 mailbox | 规划中 | 规划中 |
| Host jail 与资源隔离 | 规划中 | 规划中 |

参考验证环境为 Apple M4 上的 Linux aarch64 嵌套 KVM。当前构建产物约为：

| 产物 | 大小 |
| --- | ---: |
| VMM ELF（未 strip） | 71 KiB |
| 精简 aarch64 Linux Image | 3.7 MiB |
| Device Tree Blob | 1.4 KiB |

这些数字仅描述当前构建产物，不代表正式性能基准。启动延迟、RSS 和并发开销仍需在固定裸机环境中测量。

## 快速开始

### 环境要求

- Linux aarch64 或 Linux x86_64
- 可读写的 `/dev/kvm`
- C11 编译器与 GNU Make
- 静态 libc 工具链
- 构建 Guest kernel 时需要 Linux kernel 源码及 `dtc`、`cpio`、`gzip`

Ubuntu/Debian 示例：

```bash
sudo apt-get install build-essential bc bison flex libssl-dev libelf-dev \
  device-tree-compiler cpio gzip
test -r /dev/kvm && test -w /dev/kvm
```

### aarch64：完整 KVM 链路

kernel 构建脚本当前要求在 Linux aarch64 主机上运行：

```bash
# 1. 构建 VMM
make

# 2. 验证 KVM 基础能力
./agent-cell kvm-smoke

# 3. 构建包含 /init 和 /agent-task 的精简 Linux Image
make kernel-aarch64 KERNEL_SRC=/path/to/linux

# 4. 启动 Guest 并执行任务
./agent-cell kvm-boot build/Image-aarch64 build/virt-aarch64.dtb
```

成功运行的关键输出：

```text
kvm-smoke=ok guest-output=cell:kvm:ok
AGENT_TASK_OK
CELL_MVP_OK
kvm-boot=ok
```

### x86_64：启动接口

x86_64 使用 bzImage 和独立 initramfs：

```bash
make
make guest
./agent-cell kvm-boot /path/to/bzImage build/initramfs.cpio.gz
```

仓库目前没有提供 x86_64 kernel 配置生成脚本，因此需要调用方准备兼容的 bzImage。

## 使用案例：嵌入一个 Agent 工具

当前版本在构建期将工作负载写入 Guest。修改 [`guest/agent-task.c`](guest/agent-task.c)：

```c
#include <stdio.h>

int main(void)
{
    /* 示例：在 VM 隔离边界内执行工具逻辑。 */
    puts("{\"task\":\"policy-check\",\"status\":\"ok\"}");
    return 0;
}
```

重新生成 Guest 并启动：

```bash
make kernel-aarch64 KERNEL_SRC=/path/to/linux
./agent-cell kvm-boot build/Image-aarch64 build/virt-aarch64.dtb
```

任务 stdout 会输出到 Guest console；任务状态码决定本次执行是否成功。动态传入命令、文件、stdin 以及返回结构化结果尚未实现，后续将由有界共享内存 mailbox 承载。

## CLI

| 命令 | 说明 |
| --- | --- |
| `agent-cell selftest` | 验证控制协议的编解码与参数校验 |
| `agent-cell run ...` | 仅验证请求和生命周期状态；当前不执行 Guest 任务 |
| `agent-cell kvm-smoke` | 创建最小 VM/vCPU 并验证一次 KVM I/O exit |
| `agent-cell kvm-boot ...` | 直接启动 Linux，执行内置任务并等待完成 |

`kvm-boot` 当前固定使用 128 MiB Guest RAM 和 10 秒 deadline。

## 测试

```bash
make test       # 协议、自检、dry-run；存在 /dev/kvm 时同时运行 smoke test
make sanitize   # AddressSanitizer + UndefinedBehaviorSanitizer
```

x86_64 Guest 启动链路也可使用 QEMU/TCG 验证：

```bash
./scripts/test-qemu.sh /path/to/bzImage build/initramfs.cpio.gz
```

QEMU 测试不覆盖本项目的 KVM/VMM 边界，不能替代真实 `/dev/kvm` 测试。

## 目录结构

```text
include/cell.h                  公共数据结构与接口
src/main.c                      CLI 入口
src/protocol.c                  请求协议编解码与校验
src/supervisor.c                生命周期状态机原型
src/kvm_linux_*.c               最小 KVM smoke backend
src/kvm_boot_linux_*.c          Linux 直接启动 backend
guest/init.c                    Guest PID 1 与任务监督
guest/agent-task.c              示例工作负载
guest/virt-aarch64.dts          aarch64 虚拟平台描述
scripts/                        Guest 构建与验证脚本
docs/architecture.md            架构决策与后续设计
```

## 安全说明

KVM 提供了 Guest 与 Host kernel 之间的硬件虚拟化边界，但这并不自动使当前 Demo 成为生产级沙盒。投入真实环境前，至少还需要完成：

- 以独立非特权 UID 运行每个 VMM
- user/PID/mount/network/IPC namespace 隔离
- cgroup v2 CPU、内存和进程数限制
- seccomp syscall allowlist、capability 清理与 `no_new_privs`
- 基于 pidfd 的可靠终止和资源回收
- mailbox ABI 模糊测试、Guest 镜像度量与供应链校验
- 针对异常 KVM exit、超时及恶意 Guest 的负向测试

完整设计和演进边界见 [`docs/architecture.md`](docs/architecture.md)。

## License

Apache-2.0
