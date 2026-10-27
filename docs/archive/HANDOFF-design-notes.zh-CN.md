# sirius 交接文档 · 早期设计笔记与踩坑（2026-10-25 → 10-27）

这是 `docs/HANDOFF.zh-CN.md` 第 4 节的原始时间线：U-Boot 即固件、GIC 预配置、AppleARMCPU 抢占、
面板绘制约束等。其中若干条的结论后来被推翻（例如 ArmCpu 是否被探测），保留原文只为追溯，
**当前结论以主文档为准**。

## 关键设计与已解决问题（原始时间线）

1. **U-Boot 即固件**：跳过 Mu-Silicium 移植。UniXNU.efi/DarwinBoot.efi 走 FAT 卷文件加载（`loader = "payload"` → unixnu.toml 的 `[[driver]]`）。
2. **U-Boot env**（`~/Xnu4Sirius/unixnu-sirius.env`，`sirius-build.sh ENV_FILE=` 重编）：加 `unixnu_check` 标记检测 + bootmenu 第 2 项。`fastboot flash boot` 刷 `~/Xnu4Sirius/boot-unixnu.img`。
3. **mtools 挂死**：mtools 会开 `/dev/tty` 交互确认（已存在文件时"是否覆盖"）；无终端环境被 SIGTTIN 挂起。`make-uboot-payload.py` 用 `setsid + stdin=DEVNULL + -o` 规避。
4. **链接符号闭包**：DEVELOPMENT 内核无条件引用 `native_legacy_display_probe`（kit 提供 dormant stub）；boot-progress kit 的 extern 全部走弱定义兜底（`check-kit-symbols.py` 可复查）。
5. **BCM2837 panic 地址**：无 `XNU_DT_PANIC_REGION=y` 时内核 bzero 树莓派固定地址 0x3c0fc000 → PE_create_console 内死。defconfig 已加。
6. **stage-3/4 探针二分**：确认 MMU ✓ → serial_init ✓ → PE_create_console ✓（修复后）→ 定位到 IOKit。
7. **GIC-500 预配置（本移植原创）**：XNU Apple 版期待 iBoot 配好的中断控制器；U-Boot 不配置 → 内核停在 lockdown、IRQ=0。`drivers/DarwinBoot`（设备覆盖版）在 ExitBootServices 前按 `/arm-io/intc` 解析 DT 并镜像 ArmCpu 的 initDistributor/wakeRedistributor/enableGic 序列（含 MPIDR 匹配 redistributor、PPI 27 = 虚拟定时器）。验证：内核越过 lockdown 进入 IOKit。
   ⚠️ 2026-10-25 修正：SDM670/710 的虚拟定时器是 **INTID 19**，不是 27；并且**预配置不能提前使能 timer PPI**（见 §4.16）。
8. **AppleARMCPU 抢占**：无 `sheng-smp=1` 时 ArmCpu::probe 拒绝次级核 nub → kernelcache 里的 AppleARMCPU（IONameMatch=cpu, probe score 100）接管 → 因缺少 Apple 专有 cpu 属性 panic "CPU configuration in device tree is invalid"。修复：boot args 加 `sheng-smp=1 cpus=8 armcpu-self-ipi=1`（fajita 同款）。
9. **真实 iPad7,1 DT 提取**（`~/Xnu4Sirius/adt-dump.py` + `/tmp/j120.adt`）：IM4P→LZFSE（`~/.local/xnu-linux/bin/lzfse`）→ ADT 解析。注意属性 length 高位是 syscfg 引用标志（`& 0x7fffffff`）。
10. **设备仓 `patches/xnu/` 会整体遮蔽主仓补丁目录**：feeds.py 用 setdefault，同名 patch 模块先到先得（设备仓先注册）。设备仓一旦有 `patches/xnu/`，主仓 57 个常规内核补丁全部失效（SETUP 阶段即崩，hostcompat.h 找不到）。Linux 专用补丁放 **`patches/xnu-linux/`**（独立模块名，不遮蔽），由设备仓覆盖版 `scripts/build-xnu-4k-baseline.sh` 在常规序列之后应用，且带 `uname = Linux` 守卫。
11. **诊断面板的早期绘制约束**：`boot_progress_init()` 在 `PE_init_platform` 之后立即 `draw()`，早于同一函数后面的 `ml_parse_cpu_topology()`。面板行不能解引用尚未初始化的内核状态：TOPO 行的 `ml_get_boot_cpu_number()` 就是对 NULL `topology_info.boot_cpu` 的解引用，它把 diag 和 TOPO 两个内核都杀死在面板之前，一度被误归因为 ArmCpu 覆盖的布局平移。**已修**：TOPO 行用 `ml_get_topology_info()` 的 NULL 契约守卫（commit `ab3c9c0`）。
12. **workflow paths 过滤器**已含 drivers/**（踩了三次坑：drivers/ 或 package/ 的改动 push 后 CI 无反应）；concurrency cancel-in-progress 在构建期间 push 会取消正在跑的构建。
13. **`volatile` 自增在 CI 上被 -Werror 拒绝**：`clang` 的 `-Wdeprecated-volatile`；probe 计数器改用显式 `= x + 1`（commit `044bca3`）。
14. **面板 monitor 睡在 ArmCpu 的定时器上**：`boot_progress_start()` 起 monitor 线程，它 refresh 一次后 `assert_wait_timeout(1000 ms)`；没有定时器就一直不醒。所以 panic 后拍到的是"早一帧"的数据（PROBE 全 0 的假象）。**已修**：新增 `boot_progress_refresh_now()`（BootProgress.c），panic screen 在清屏前通过 symbol 解析的绝对地址调用它（commit `4c62c99`；payload 内核里可见 `blr` 到 `0xfffffff007972c38`），第 2/3 行因此是 panic 时快照。
15. **AppleARMCPU 的校验逻辑（逆向）**：kernelcache 里 `AppleARMCPU::_validateConfiguration()` 读取 `/cpus/max_cpus`（长度 4）、比较实际 CPU 数 ≤ max、并检查 `function-ipi_dispatch` vs `no-aic-ipi-required`；缺失即 `_validateConfiguration()` 返回 0 → `AppleARMCPU::start` 调 `panic("CPU configuration in device tree is invalid")`（`AppleARMCPU.cpp:111`）。相关地址：panic helper `0xfffffff0049bcfc0`（__PLK_TEXT_EXEC），调用点 `0xfffffff004986a54`，校验函数 `0xfffffff004986ac4`。它的 personality：`IOClass=AppleARMCPU; IOProviderClass=IOPlatformDevice; IONameMatch='cpu'; IOProbeScore=100`（来自 `__PRELINK_INFO` 的 AppleARMPlatform kext）。
   原生 ArmCpu 的 personality 在主仓 `patches/xnu/armvirt-platform-personality.patch` 注入的 `gIOKernelConfigTables`：`IOClass=ArmCpu; IOProviderClass=IOPlatformDevice; IOProbeScore=2000`（无 IONameMatch，应匹配所有 IOPlatformDevice）。内核里 `_ZN6ArmCpu10gMetaClassE`、`_ZN6ArmCpu5probeE...` 等符号齐全，配置表字符串也在；**但截至最新一次真机快照，`ArmCpu::probe` 是否被调用尚无定论**（见 §2 的警告）。
16. **SDM670/710 的 arch timer 映射（来自 `~/sirius-distro` 主线 DTS）**：`sdm670.dtsi` 的 timer 节点是 `GIC_PPI 1/2/3/0`（secure phys / non-secure phys / virtual / hyp），所以 **虚拟定时器 = PPI 3 = INTID 19**，不是 Apple 的 PPI 11/INTID 27；fajita 移植早就用 `armcpu-timer-intid=19`。已加 board.toml `armcpu-timer-intid=19`，ArmCpu 驱动参数化（`gTimerIntid`），DarwinBoot 预配置也改成 19。
   ⚠️ **新陷阱**：把 DarwinBoot 预配置改成使能 19 后，定时器中断在 ArmCpu::start 之前就到达，`_sleh_irq` 的每 CPU IRQ handler 还是 NULL → `blr x8` 跳到 0，指令取指异常 **pc=0 / lr=0xfffffff0073ef87c**。面板 `last IRQ PC` 从 0 变成 `0xfffffff0073f3478`，也证明 INTID 19 确实能进中断。下一步应把预配置里的 timer PPI 位撤掉（只保留 SGIs），让 ArmCpu 在自己的 handler 就绪后再使能 19。
17. **设备树数值核对**：主仓 ADT 生成器的 CPU reg（`0x0,0x100…0x700`）与 GIC（`0x17a00000/0x17a60000`，`redistributor-stride=0x20000`）与 `~/sirius-distro/kernel/linux-sirius/arch/arm64/boot/dts/qcom/sdm670.dtsi` 完全一致；CPU 拓扑（8 核、boot=0）也在面板 TOPO 行得到确认。不要把 Apple/Qualcomm 的 timer PPI 编号混用。

