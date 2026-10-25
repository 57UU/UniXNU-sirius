# UniXNU on Xiaomi Mi 8 SE (sirius) — 交接文档

更新：2026-10-25。本文档交接"把 UniXNU（XNU 内核 + iPadOS 用户态）移植到小米 8 SE"的全部上下文：当前进度、架构决策、构建/部署循环、已解决的问题、下一步路线。

---

## 1. 项目概况

| 项 | 内容 |
| --- | --- |
| 目标设备 | 小米 8 SE（sirius，Snapdragon 710 / Kryo 360，6GB eMMC 机型） |
| 目标系统 | [UniXNU](https://github.com/UniXNU/UniXNU)：XNU 23.6.0（4KiB BCM2837 personality）+ 预链接 iPad7,1 iPadOS 17.7.11 (21H461) 内核缓存 |
| 设备仓库 | https://github.com/57UU/UniXNU-sirius（本仓库；CI 在此构建内核） |
| 本机工作区 | WSL `~/Xnu4Sirius/`：`work/UniXNU`（主仓库检出）、`UniXNU-sirius`（本仓库）、`payload-full.img` 等产物 |
| 参考端口 | UniXNU-fajita（OnePlus 6T，SDM845 —— 最接近的同代 SoC）、UniXNU-sheng |
| 相关资产 | `~/sirius-distro`（手机上的 Linux 发行版 + 主线内核 DTS + U-Boot 源码）、`~/rocky-sdm710-linux`（安卓内核）、`~/Xnu4Sirius/work/uefi-sheng`（Mu-Silicium，Font.h 来源） |

**本移植的核心架构决策：用设备自带 U-Boot（sirius-distro, U-Boot 2026.07, EFI_LOADER 全开）替代 Mu-Silicium 固件**。引导链：

```
ABL (boot 分区 = 我们重编的 U-Boot boot.img)
  └→ U-Boot UEFI (GOP/vidconsole + FAT 块设备 + EFI_LOADER)
      └→ 标记检测: p80 (vendor 分区 FAT) 有 /EFI/UniXNU/BOOTUNIXNU 就启动 UniXNU，否则正常启动 Linux
          └→ \EFI\BOOT\BOOTAA64.EFI = UniXNU.efi（菜单）
              ├→ unixnu.toml 配置（payload 卷根）
              ├→ [[driver]] DarwinBoot.efi（XNU 加载器，本仓库 drivers/DarwinBoot 覆盖版）
              │    ├→ GIC-500 预配置（iBoot 角色，见 §4）
              │    └→ 加载 kernel.macho（预链接 XNU）+ kernel.macho.adt → ExitBootServices → DarwinEnter
              └→ XNU → boot-progress 面板（大字体实时内核日志）
```

## 2. 当前状态（2026-10-25）

| 阶段 | 状态 | 证据 |
| --- | --- | --- |
| S1 UEFI（U-Boot） | ✅ | UniXNU 菜单/驱动加载在真机验证 |
| S3 payload 卷 | ✅ | p80 FAT + 标记自动引导 |
| S4 内核入口 | ✅ | 绿/蓝/品红探针色带 |
| S5 MMU/定时器/PSCI | ✅ | stage-3 复位循环（20s 定时 + PSCI 复位） |
| S6 控制台 | ✅ | boot-progress 面板；panic 时刻也能刷新快照（§4.14） |
| S7 IOKit | 🔶 卡在 AppleARMCPU | 面板 TOPO 行证明内核拓扑正确（`boot=0 max=7 nums=01234567`）；原生 ArmCpu 是否被 IOKit 探测仍未取得可信快照 |
| S8 root device | ⏳ | 无存储驱动（eMMC 机型，无 UFS）；预期停在 root 等待/panic |

**最新部署（2026-10-25 00:17）**：

- 内核：CI run `40108927790`（commit `4c62c99`），源内核 `ee403bb1…`，payload 内补丁后 `6f204e3c…`
- DarwinBoot：`a1ed1a39…`（GIC 预配置改为 INTID 19，见 §4.16）
- `payload-full.img`：`d471b716…`（已刷 vendor p80 并重启）
- 手机不在 fastboot、也没回落 Linux；屏上是 boot-progress 面板 + panic 文字。

**真机屏上实测（照片/OCR，2026-10-25）**：

```
TOPO boot=0 max=7 nums=01234567|DT cfg=0 cpu=…        ← 内核拓扑完全正确
PROBE calls=0 acc=0 dtype=0 dnum=0 smp=0 num=…        ← 见下方警告
last IRQ PC=fffffff0073f3478 PSR=20000204 SP=…        ← 不再是 0（定时器中断已进入 sleh_irq）
stage: pid=0 early_boot
Kernel instruction fetch abort at pc 0x0, lr 0xfffffff0073ef87c
```

- `TOPO` 行可信：`boot_cpu`/`max`/8 个 `ml_get_cpu_number` 全部正常，`BootProgress` 的 NULL 守卫修复有效。
- `PROBE calls=0` **目前不可作为结论**：这次 boot 在 `early_boot` 阶段就被定时器中断里的空函数指针打断（见下），很可能还没走到 CPU nub 的 IOKit 匹配。需要先修掉提前使能 timer PPI 的问题再取一次快照。
- `lr 0xfffffff0073ef87c` 落在 `_sleh_irq+0x70`：`blr x8` 时 `x8=[x21,#0xc8]` 为 NULL，即 **CPU 的 IRQ handler 还没注册**（ArmCpu::start 尚未跑），而 `DarwinBoot` 已经在预配置里使能了 PPI 19，定时器中断提前到达 → pc=0 取指异常。
- 屏最上面两行（标题/time）被圆角/刘海遮挡；TOPO/PROBE 在第 2/3 行也被挡了一部分。下次诊断行应放到 **第 8/9 行**（panic 清屏线 `height/8` ≈ 第 20 行，scale=1 时）。

## 3. 构建-部署-测试循环

### 3.1 内核（改 defconfig/kit/补丁/驱动时需要 —— macOS CI）

```bash
cd ~/Xnu4Sirius/UniXNU-sirius
git add -A && git commit && git push          # push 到 main 自动触发（注意 paths 过滤器）
# 若 paths 不覆盖（如 package/ 之外的杂项）手动触发：
gh workflow run kernel.yml --ref main -f steps=kernel
# 等待 ~8-10 分钟，产物下载：
gh run download <run-id> -D ~/Xnu4Sirius/kernel-artifact/tmp -n sirius-kernel-<N>
```

⚠️ 网络不稳：`gh run download` 经常 connection reset / 静默不下载；直连 `git push` 也可能 `GnuTLS recv error`。可靠做法是走 Windows 宿主代理（WSL 里宿主机网关 = 172.23.0.1；可用 `ip route | grep default` 确认）：

```bash
export https_proxy=http://172.23.0.1:7900 http_proxy=http://172.23.0.1:7900
git push origin main

TOKEN=$(gh auth token)
URL=$(curl -s -I -H "Authorization: Bearer $TOKEN" \
  "https://api.github.com/repos/57UU/UniXNU-sirius/actions/artifacts/<ID>/zip" \
  | tr -d '\r' | sed -n 's/^[Ll]ocation: //p')
wget -O /tmp/art.zip "$URL"          # 必须走代理；402/403 时重新取 Location
python3 -m zipfile -e /tmp/art.zip /tmp/art/
```

⚠️ workflow 的 concurrency 是 `cancel-in-progress: true` —— 构建期间**不要 push**（会取消正在跑的构建）。构建失败时用 `gh run view <RUN> --log-failed` 看失败步骤。

### 3.2 EFI 程序（本地 WSL，秒级）

```bash
cd ~/Xnu4Sirius/work/UniXNU
./unixnu build package        # UniXNU.efi（已构建过，一般不用重跑）

# DarwinBoot.efi（本仓库覆盖版，含 GIC 预配置）：
cd ~/Xnu4Sirius/UniXNU-sirius/drivers/DarwinBoot
BASE=$HOME/Xnu4Sirius/work/UniXNU/out/feeds/mu_basecore-89034cbb
WORKSPACE=$PWD PACKAGES_PATH=$PWD:$HOME/Xnu4Sirius/work/UniXNU/package:$BASE \
EDK_TOOLS_PATH=$BASE/BaseTools \
CONF_PATH=$HOME/Xnu4Sirius/work/UniXNU/out/sirius/package/Build/Conf \
CLANG_BIN=/usr/bin/ PYTHON_COMMAND=python3 \
PATH=$BASE/BaseTools/BinWrappers/PosixLike:$BASE/BaseTools/Source/C/bin:$PATH \
build -p DarwinBootPkg/DarwinBoot.dsc -a AARCH64 -b DEBUG -t CLANGPDB -n 32
# 产物: build/DarwinBoot/DEBUG_CLANGPDB/AARCH64/DarwinBoot.efi（~7 秒）
```

### 3.3 payload 组装（本地，约 1 分钟）

```bash
cd ~/Xnu4Sirius/work/UniXNU
UNIXNU_UEFI=$HOME/Xnu4Sirius/work/uefi-sheng \
./unixnu build payload --kernel ~/Xnu4Sirius/kernel-artifact/kernel-<name>.macho
python3 ~/Xnu4Sirius/UniXNU-sirius/scripts/make-uboot-payload.py \
  --payload out/sirius/payload/payload.img \
  --unixnu-efi out/sirius/package/Build/Out/AARCH64-DEBUG/UniXNU.efi \
  --darwinboot-efi ~/Xnu4Sirius/UniXNU-sirius/drivers/DarwinBoot/build/DarwinBoot/DEBUG_CLANGPDB/AARCH64/DarwinBoot.efi \
  --marker --output ~/Xnu4Sirius/payload-full.img
```

- `--marker` 写入 BOOTUNIXNU（自动启动 UniXNU；去掉则开机默认 Linux）。
- `UNIXNU_UEFI` 提供 panic-screen 探针需要的 Font.h（uefi-sheng 检出）。
- `unixnu build payload` 会重装 `out/sirius/tree`（脚本从设备仓覆盖主仓）；payload patch 会打印 `panic screen: ...`，并会做指令级校验，偏移不符会直接拒绝。

### 3.4 真机部署

```bash
# 手机侧进入 fastboot：UniXNU 菜单选 Power off → 音量下+电源；Linux 里 sudo reboot bootloader
# Windows 侧（Git Bash）：
fastboot.exe flash vendor "//wsl.localhost/Ubuntu-26.04/home/u57u/Xnu4Sirius/payload-full.img"
fastboot.exe reboot
```

恢复 Linux：fastboot 下 `fastboot flash boot ~/sirius-distro/build/boot.img`（原版 U-Boot，不认标记，直接进 Linux），操作完再把 `~/Xnu4Sirius/boot-unixnu.img` 刷回去。手机回到 Linux 后 SSH：`ssh u57u@172.16.42.1`（免密已铺）。

### 3.5 观察输出

屏幕即调试器：boot-progress 面板（CPU/SMP/CLK/MEM/GIC/TOPO/PROBE 各行 + stage 时间线 + 内核 console 末行）。panic 会以大字画屏。探针（board.toml `entry_probe_*`）可做二分复位定位。

面板布局注意（实测）：

- GOP 宽度 1080 → `display.scale=1`，每行 14 px，48 行占 672 px。
- panic screen 从 `height/8`（2244/8 = 280 px ≈ 第 20 行）往下清屏；**第 0..19 行在 panic 后仍可见**。
- 屏顶部有圆角/刘海，**不要把关键数据放 0..3 行**；推荐第 8/9 行。
- monitor 线程只画一帧就 `assert_wait_timeout`（依赖 ArmCpu 的定时器）；没有定时器时面板会冻住。panic 时刻的可信快照靠 §4.14 的强制刷新。

## 4. 关键设计与已解决问题（时间线）

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

## 5. 下一步路线（按优先级）

1. **撤掉 DarwinBoot 预配置里的 timer PPI 使能**（`GIC_SGI_PPI_ENABLE` 只保留 SGIs 0..15；或恢复成 27 这种不会触发的值），保留 board.toml 的 `armcpu-timer-intid=19` 和驱动参数化；重建 DarwinBoot、重装 payload、重刷。目标：回到 `AppleARMCPU` 的 CPU-config panic（而不是 pc=0 abort），且 panic 快照里的 PROBE 是"CPU 匹配之后"的值。
2. **确认 ArmCpu personality 是否被 IOKit 探测**：
   - 若 PROBE `calls>0`：看 `dtype/dnum/num/boot` 定位是属性检查还是拓扑号问题；
   - 若仍 `calls=0`：ArmCpu 的 built-in personality 没有匹配。排查方向：`gIOKernelConfigTables` 的解析/类注册时序；对照 fajita 能工作的内核（同 patch、同 score）；必要时改用 `__BUILTIN,__info` 段注入 personality（`libsa/bootstrap.cpp::readBuiltinPersonalities` 支持整 kext 链入）或一个 codeless prelink kext 给 ArmCpu 挂 `IOPlatformDevice` 匹配。
3. **面板诊断行位置**：把 TOPO/PROBE 从第 2/3 行移到第 8/9 行（避开刘海；panic 清屏线 ≈ 第 20 行，仍然安全）。
4. **SMP/timer**：ArmCpu 抢到 boot CPU 后，验证 `SMP run=7`、PPI 19 的 `IRQ` 增长、stage 推进，再回到 AppleARMCPU 抢次级核的问题。
5. **root device 阶段**：预期 panic/wait。可选：加 `XNU_SHENG_PMGR`（AMFI 需要 PMGR，见 fajita `build-sheng-platform-power.py`，注意它需要 `build/native-prelink/` 的参考候选文件）。
6. **显示**：`XNU_NATIVE_LEGACY_DISPLAY`（CPU 渲染显示，fajita 的 `build-native-legacy-display.py` + `drivers/NativeMobileDisplay` 覆盖）。屏参：1080x2244、stride 4320、fb 0x9c000000（GOP 宽度 1080 → 面板 scale=1）。
7. **触摸**：sirius 触摸 IC 是 SYNAPTICS S3706（主线 DTS `synaptics,s3706`，GENI I2C @0xa94000?）—— 参考 fajita 的 S3706B 驱动（NativeRmi4Touch + NativeGeniI2c）。
8. **存储（eMMC！）**：`sdhc_1: mmc@7c4000`（查 `sdm670.dtsi`）。NativeUfs 不适用；需要新驱动（NativeSdhci，参考 drivers/NativeUfs + 主线 `drivers/mmc/host/sdhci-msm.c`），或先用 NativeRamBlock 做 RAM Data 卷。
9. **用户态**：trust cache、data seed、`unique_chip_id`（board.toml [payload.adt] 需成对加 chip_id/unique_chip_id）、IPSW OS 卷、launchd…
10. **本地内核编译（工具链已全通，首跑进行中）**：`~/Xnu4Sirius/xnu-linux-toolchain`（Allenwdk）。bootstrap 全部完成，途中坑：unifdef 需 `CC=clang`；tcsh 链接需 `make LIBES='-lcrypt -lncursesw -ltinfo'`；cctools-port 需 libdispatch（swift-corelibs-libdispatch swift-5.10-RELEASE）。用法：`source env.sh && ./unixnu build kernel`。要求网络可达 GitHub API；不要在 run_in_background 的管道 stdin 下跑（migcom 死锁）。预期剩余缺口：firehose 私有头。

## 6. 环境清单

- **WSL**（Ubuntu 26.04, 32 核）：git/clang21/lld/mtools/dtc/unifdef/cmake/ninja/g++/flex/bison、`gh`（账号 57UU，已登录）。
- **Windows 侧**：`fastboot.exe`（Git Bash 直接用；wsl 路径用 `//wsl.localhost/Ubuntu-26.04/...`）。
- **手机**：boot 分区 = unixnu U-Boot；vendor(p80) = payload FAT；system(p79) = Linux 内核（勿动）；userdata(p81) = Linux rootfs。SSH u57u@172.16.42.1（密钥已铺，sudo 密码 1234）。
- **CI**：GitHub Actions macos-26 runner（公开仓库免费），产物 `sirius-kernel-<N>`，retention 3 天，及时下载。
- **网络**：WSL 默认网关 172.23.0.1；GitHub API/对象存储经常需要 `https_proxy=http://172.23.0.1:7900`。

## 7. 常见坑速查

| 症状 | 原因/解法 |
| --- | --- |
| mtools 命令永久挂起 | `/dev/tty` 交互 + SIGTTIN；`setsid` + `< /dev/null` + `-o` |
| fastboot 显示 `????????????` | USB 会话坏；拔插线或重启手机进 fastboot |
| CI 没触发 | push 的路径不在 workflow paths 过滤器；手动 `gh workflow run` |
| CI 构建被取消 | concurrency cancel-in-progress；构建期间别 push |
| `git push` 报 GnuTLS recv error | 走宿主代理：`https_proxy=http://172.23.0.1:7900 git push` |
| `gh run download` 静默失败/超时 | 用 API 取 302 Location + 代理 wget（§3.1） |
| payload 补丁报 "padding ends at 0x..." | stage-3 洞 + panic screen 超出内核 padding；二分时关 panic_screen |
| 内核停在 lockdown、IRQ=0 | GIC 未预配置（DarwinBoot PrepareGic，本仓库已含） |
| 屏幕只有色带没日志 | boot-progress 未编入或 `boot-progress=2` 参数缺失 |
| 面板只有顶部几行、下面被大字覆盖 | 这是 panic screen；第 0..19 行是面板，第 20 行起被清屏 |
| TOPO/PROBE 行被刘海挡住 | 诊断行放第 8/9 行（不要放 0..3） |
| PROBE 全 0 但 CPU 匹配已发生 | 面板 monitor 睡在定时器上；需要 panic-time refresh（§4.14） |
| `pc=0 / lr=...3ef87c` 取指异常 | `_sleh_irq` 的 CPU IRQ handler 为 NULL；不要提前使能 timer PPI（§4.16） |
| “CPU configuration in device tree is invalid” | AppleARMCPU 抢到 cpu nub 且 `/cpus/max_cpus` 校验失败；根因在 ArmCpu 没匹配（§4.15） |

## 8. 仓库结构（本仓库）

```
board.toml                        # 板级配置（内核参数、payload、ADT 参数、探针）
configs/sirius_defconfig          # 内核配置（第一控制台 + boot progress + DT panic region）
drivers/ArmCpu/                   # 覆盖版 ArmCpu（probe 计数器 + timer INTID 参数化；其余与主仓一致）
drivers/DarwinBoot/               # 覆盖版 XNU 加载器（GIC 预配置）
package/                          # kit：boot-progress 组件 + dormant stubs + Font.h
scripts/
  make-sirius-adt.py              # Apple DT 生成器（SDM710 值）
  make-uboot-payload.py           # U-Boot 路线 payload 终装
  build-boot-progress.py          # boot-progress 归档构建
  build-sirius-probes.py          # dormant probe stub 归档
  build-xnu-4k-baseline.sh        # 覆盖版（放宽 boot-progress 的 UFS interlock）
  patch-xnu-entry-probe.py        # 入口探针 + panic screen（含 boot_progress_refresh_now 钩子）
  xnu-panic-screen.c              # panic 大字屏（清屏 / 寄存器 / 快照刷新）
.github/workflows/kernel.yml      # macOS 内核 CI
docs/                             # linux-kernel-build.md、本文档
```

## 9. 近期提交（本仓库）

```
4c62c99 boot: snapshot the probe rows at panic time and use the SoC timer PPI
044bca3 ArmCpu: use non-deprecated volatile increments in the probe counters
3f190f8 ArmCpu: expose the /cpus probe decision above the panic-screen clear line
62fccca docs: correct the TOPO-row root cause (early draw, not the ArmCpu override)
ab3c9c0 boot progress: do not read the CPU topology before it is parsed

注：560f772 曾撤掉 ArmCpu 覆盖（当时误判为对象布局平移）；3f190f8 重新引入覆盖并加 probe 计数器。
```

## 10. 2026-10-25 晚：format 2 内核首次跑到 `bsd_init`（8 核在线）

这一轮把"蓝条之后无紫条"一路推到 AMFI，硬件证据（面板 OCR）如下：

```
time=31s idle=30s IRQ=1076 monitor=30 lost=0 root=0 procs=0 free=44514
TOPO boot=0 max=7 nums=01234567
PROBE calls=17 acc=8 dtype=9 dnum=0 smp=1 num=7 boot=0 reg=700 tl=8 rl=32
stage: pid=0 kernel_task bsd_init; calling bsd_autoconf?
panic: "AMFI: No PMGR?\n" @ConfigurationSettings.cpp:235
```

### 这一轮修掉的四个真问题

1. `CONFIG_XNU_NATIVE_ARCH_TIMER=n`（见 §4 的补记）：构建脚本用
   `NATIVE_ARCH_TIMER=${XNU_NATIVE_ARCH_TIMER:-${XNU_ARMCPU:-0}}`，显式 0 会把 BCM2837 的
   本地中断路由写编进 `start.s`，SDM710 上该地址为空 → MMU 前同步异常。defconfig 已删除该覆盖。
2. **DarwinBoot 提前使能 timer PPI**：`GIC_SGI_PPI_ENABLE` 含 bit19，`ArmCpu::start` 的每 CPU
   handler 还没装好，中断先到 → `_sleh_irq+0x70` 的 `blr x8` 跳 0。现在只使能 SGI 0..15，
   INTID 19 交给 ArmCpu（`armcpu-timer-intid=19`）。
3. **AppleARMPlatform 的 CPU 类抢占 `/cpus`**：`IONameMatch = "cpu"` 在
   `IODTPlatformExpert` 里是**前缀匹配**，所以 `cpu0..cpu7` 全中；它的 `start` 校验 Apple 形状的
   `/cpus`（`max_cpus`/`cpu-id`/`interrupts`/`function-*`）后 panic。原生 ArmCpu 的 personality
   （`IOPlatformDevice`，probe score 2000）即使 probe 里再加 40000 也抢不过它，所以
   `scripts/make-uboot-payload.py` 直接把 personality plist 里的 `IONameMatch` 从 `cpu` 改成
   `xpu`（等长替换，镜像内偏移不变）。
4. **`ArmCpu::initDistributor` 覆盖了 DarwinBoot 的 GIC 状态**：它重跑
   `GICD_CTLR &= ~3` → 等待 → `|ARE` 的静默序列，清掉了 group 使能，核心此后没有任何中断，
   面板定格在最后一秒（**面板定格 ≠ 死等**，面板靠定时器中断重绘）。现在若
   `GICD_CTLR` 已同时有 ARE 与 group1，就保留固件状态，只静默 SPI。

### 调试工具（都是这一轮加的）

- `armcpu-panic=N`：在 `ArmCpu::start` 的第 N 个检查点 panic（1 入口 / 2 prepareGic / 3 注册 /
  4 中断控制器 / 5 initCPU / 6 registerService / 7 mapGic / 8 initDistributor /
  9 wakeRedistributor / 10 IOCPU::start），用来二分"面板定格"到底停在哪。
- 面板：字号 1080 宽用 2 倍（12×28），行起始下移 `height/24` 让开刘海。

### 下一个卡点：AMFI 的 PMGR

`AMFI: No PMGR?` 发生在 `ConfigurationSettings.cpp:235`。未修改的目标 AMFI 会：

1. `IOService::serviceMatching("ApplePMGRNub")` + `waitForMatchingService`；
2. `nub+0x88` 取 provider，调用其 `readAon`（vtable 槽 320）/ `writeAon`（槽 321），读值 `|4` 再写回。

原始类尺寸：`ApplePMGR` = 0x4b8a8，`ApplePMGRNub` = 0x740。
`UniXNU/scripts/audit-amfi-pmgr-contract.py` 把这些指令地址钉死了，可以直接对照。
`amfi_get_out_of_my_way=1` **不能**跳过这段。

两条路线：

- **A（已验证过的思路）**：照 `UniXNU-sheng` 的做法，用
  `OSKextLoadKextWithIdentifier("com.apple.driver.ApplePMGR")` 载入原 kext，自己按生成的
  `OriginalPMGRABI.h` 子类化 `ApplePMGR`（`readAon`/`writeAon` 返回 0xffffffff），再造一个
  `ApplePMGRNub` 发布为服务；sheng 用 `sheng-pmgr=2` 开关，并且 `payload.py` 在
  `native-ufs-root=1` 时会强制要求它。
  注意：sheng 仓库里**没有** `sheng_original_pmgr_nub_create/read/write` 的实现，需要按上面的
  契约自己写（或继续挖它的 kit）。
- **B（更快但更脏）**：在 prelinked 内核里 patch 掉 AMFI 这段（跳过查询/读取，AON 取 0xffffffff）。
  已知锚点：panic 字符串在 `0xfffffff0042dde2d`，其 adrp/add 引用在
  `0xfffffff004ac9414`，对应 helper 里 `mov w9, #235`；AMFI 的 `__TEXT_EXEC` 在
  `0xfffffff004aaa000`。

### 设备身份（重要）

刷机前必须核对：`fastboot getvar product` 应为 **`sirius`**、序列号 **`e4b6c475`**。
本轮曾有另一台设备（`product: rubens`，序列号 `INY58TDYPVJJB6AE`）连在同一台主机上，
误刷会覆盖那台的 vendor 分区（用其原厂 `vendor.img` 恢复）。
