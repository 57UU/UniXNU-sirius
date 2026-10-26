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

### AMFI 已打通（2026-10-25 深夜）与下一个卡点

**不用改目标内核**：AMFI 那两条 thunk 是

```asm
ldr x0, [x0, #0x88]   ; provider = nub->pmgr
cbz x0, +0x10         ; NULL 时 readAon 返回 0、writeAon 直接返回
```

所以只要发布一个**真正的 `ApplePMGRNub` 实例、但把 provider 留空**，AMFI 就会读到 0、`|4`、写空，
latch 正常完成。实现是 `package/SiriusPmgr.cpp`：

1. `OSMetaClass::getMetaClassWithName("ApplePMGRNub")` → `alloc()` → `init()`；
2. attach 到 `IOService::getPlatform()` 后 `registerService(kIOServiceSynchronous)`；
3. 自己用 `serviceMatching` + `waitForMatchingService` 验证并 `IOLog`（`sirius-pmgr: ApplePMGRNub size=1856 ...`）。

挂接点是 `sheng-platform-power.patch` 在 `bsd_init` 里的 `sheng_pmgr_run()`（`CONFIG_XNU_SHENG_PMGR=1`，
启动参数 `sheng-pmgr=2`）。**构建脚本必须把归档写到
`build/sheng-platform-power/libShengPlatformPower.a`**：`build-xnu-4k-baseline.sh` 只 force-load 这个路径。

硬件结果：

```
time=61s idle=60s IRQ=2145 monitor=60 root=0
TOPO boot=0 max=7 nums=01234567 / PROBE acc=8 num=7 reg=700
stage: bsd_init; calling setconf? -> bsd_autoconf?
panic: Failed to mount root device @IOKitBSDInit.cpp:984
```

也就是说：I/O Kit、8 核 SMP、timer、AMFI 全部走通，现在缺的是**块设备 + 根文件系统**。

下一步选项：

1. **UFS 驱动**：SDM710 是 UFS 2.1，参考 `UniXNU/ShengUfs`（sheng 的 NativeUfs）移植 ufshcd + PHY +
   SCSI/SD 层；
2. **VirtIO 块**：如果有可用的 virtio-mmio 传输（sheng 用过 `VirtioMmioBlock`），可以从 payload
   卷或另一块内存盘提供根；
3. **ramdisk 根**：做一个 APFS/只读根镜像塞进 payload，`rd=` 起，先验证用户态；
4. 同时建议补上 USB CDC-ACM 串口（见 §10 开头引用的 picasso PR），这样 root 阶段的日志不再靠拍照。

### 2026-10-26 凌晨：USB 控制台已链接，SMP 从核其实没起来

面板（`time=842s`，`sirius_usb_hold=1` 把内核停在 `bsd_init`）：

```
stage: pid=0 bsd_init enter          IRQ=30775 monitor=829
SMP req=1 run=0 cond=1 boot=0 | c1=step4 c2=step4 ... c7=step4
TIMER ctl=1   GIC stride=20000 typer.lo=21
```

两条结论：

1. **面板的 `Kernel console` 区是早期快照**（一直停在 iBoot / IONVRAM 那几行），
   所以"没有 `SIRIUS USB:` 输出"**不能**推断驱动没跑；要把驱动的状态放到
   boot-progress 的行里（像 `armcpu_probe_*` 那样导出全局，由 `package/BootProgress.c` 打印），
   否则 USB 控制台和面板互相等，没法诊断。
2. **SMP 从核没有真正启动**：`armcpu_smp_step[]` 全部停在 4（已发 PSCI CPU_ON），
   `run=0`、没有任何核到 5（进入 initCPU）。之前面板的 `PROBE acc=8` 只说明
   ArmCpu 注册了 8 个 nub，不等于核起来了。这是独立于 USB 的一个真问题，
   下一步要查 PSCI CPU_ON 的返回值（`armcpu_smp_psci_rc[]`）和复位向量页的清理范围。

USB 侧的当前事实（来自本仓库的 Linux 源码 `sirius-distro/kernel/linux-sirius`）：

| 项 | 值 |
| --- | --- |
| DWC3 | `0xa600000`，reg 大小 `0xcd00` |
| wrapper (QSCRATCH) | `0xa6f8800`（= DWC3 + 0xf8800） |
| PHY | QUSB2 `0x88e2000`，**只有 USB2**，无 SuperSpeed，无需 QMP |
| apps_smmu stream id | **`0x740`**（移植源自 SM7250 的 `0xe0`，这是必须改的一处） |

构建集成：驱动在 `kits/sirius/sirius_usb.c`，由 `scripts/build-sirius-usb.py` 编成对象并加入
`build/sheng-platform-power/libShengPlatformPower.a`（该归档由 `CONFIG_XNU_SHENG_PMGR=1`
force-load）；XNU 补丁只保留 `bsd_init.c` 启动调用和 `serial_console.c` 镜像钩子。
**不要**再往 `bsd/conf/files` 加新文件：由 conf/files 生成的 makefile 不会包含补丁新增的文件，
对象永远不会被编出来，而符号表却会引用它。

### USB 卡点已定位：GCC 把 USB 的 GDSC 关了（2026-10-26 00:20）

面板新增一行（`package/BootProgress.c` 第 29 行，数据来自 `kits/sirius/sirius_usb.c` 导出的
`sirius_usb_step/gcc/smmu/dwc/dsts`）：

```
USB step=2 gcc=00222001 smmu=0 gsnpsid=00000000 dsts=00000000
SMP req=1 run=7 cond=1 boot=0 | c1..c7=step7
```

- `step=2`：GCC 映射成功、寄存器读到，然后**在电源门检查处退出**。
- `gcc=0x00222001`：bit31 = 0（**未上电**），bit0 = 1（**SW_COLLAPSE 已置位**）。
  驱动按设计不碰未上电的控制器，所以 SMMU/DWC3 都没走到（`smmu=0`、`gsnpsid=0`）。
- 偏移不是错的：`gcc-sdm845.c` 里 `usb30_prim_gdsc.gdscr = 0xf004`，与本移植的
  `GCC_USB30_PRIM_GDSCR 0x0f004` 一致（SDM670/710 与 SDM845 同族）。
- **SMP 这次全起来了**（`run=7`，7 个从核都在 `step7`）。之前那次 `step4` 是偶发，
  与 USB 无关；说明 PSCI/复位向量那条路是通的。

下一步（USB）：像 Linux 的 `gdsc_enable` 那样自己上电——

1. `GDSCR(0xf004)`: 清 bit0（SW_COLLAPSE），轮询 bit31 直到置位（有界，例如 100 ms）；
2. 若 bit31 仍为 0，说明父域（CX）没电，需要 RPMH/regulator，那时再评估；
3. 上电后依次打开 `CFG_NOC_USB3_PRIM_AXI`、`USB30_PRIM_MASTER`、`AGGRE_USB3_PRIM_AXI`、
   `USB30_PRIM_SLEEP`、`USB30_PRIM_MOCK_UTMI` 的 CBCR（清 bit0，轮询 bit31 == 0），
   以及 `USB3_PRIM_CLKREF`；
4. QUSB2 PHY（`0x88e2000`，仅 USB2）：按 `qusb2-v2-phy` 的初始化序列配置并上电；
5. 再让 DWC3（`0xa600000`）走 reset → device mode。

建议先把 1 做成一个 `sirius_usb_power=1` 的实验开关，只看 `gcc` 从 `0x222001` 变成
bit31=1 的什么值，就能判断父域是否还在。

### USB 电源实验结论（2026-10-26 00:45）

通过 `sirius_usb_power=N` 逐步试出来：

| N | 动作 | 结果 |
| --- | --- | --- |
| 1 | 清 GDSCR 的 SW_COLLAPSE，轮询 200ms | **安全**。读回 `0x00222001 -> 0xf8222000`，bit31 置位：**父域 CX 还在，纯软件可上电** |
| 2 | 上电后继续读时钟状态 | **安全**。`clk=80000000`，说明有分支时钟 halt |
| 3 | 打开 `0xf00c/0xf010/0xf014/0xf04c` 四个分支时钟后继续 | **挂死并重启**（`time=1`） |
| 4 | 同上但先检查 halt 位，未清零就停手 | **仍然挂死重启** |

结论：**挂死发生在分支时钟寄存器的访问上，不是 DWC3**。SDM710 的 GCC 在父时钟（RCG/PLL）没运行
时被访问会把总线拖死，所以必须按 Linux 的顺序先把时钟源配好再开分支：

1. 确认 `GPLL0` 在跑（`USB30_PRIM_MASTER_CLK_SRC` 的父时钟）；
2. 配 `USB30_PRIM_MASTER_CLK_SRC`（`cmd_rcgr = 0xf018`）与 `USB30_PRIM_MOCK_UTMI_CLK_SRC`
   （SDM845 是 `0xf030`），走 `clk_rcg2` 的 RCG_CFG/RCG_MND 流程；
3. 再开 `gcc_usb30_prim_master_clk`(0xf00c)、`sleep`(0xf010)、`mock_utmi`(0xf014)；
4. QUSB2 PHY（`0x88e2000`）上电；
5. 最后 DWC3。

注意：`GCC_CFG_NOC_USB3_PRIM(0xf07c)` 和 `GCC_AGGRE_USB3_PRIM(0xf080)` 这两个偏移是 SM7250 的，
SDM845 同族里 `cfg_noc_usb3_prim_axi_clk` 在 `0x502c`、`aggre_usb3_prim_axi_clk` 在 `0x8201c`，
SDM710 很可能也不同，不要再拿它们做门禁（`power>=3` 已改为只报告）。

默认命令行已去掉 `sirius_usb_power`，手机回到"停在 bsd_init"的安全状态。

### USB 时钟链已打通，卡在 EP0 命令（2026-10-26 01:15）

驱动现在自动完成整套上电（`package/sirius_usb.c`，带 halt 检查，任一步失败就停手）：

```
GDSC 0xf004 -> cfg_noc 0x502c -> aggre 0x8201c -> master 0xf00c -> sleep 0xf010 -> mock_utmi 0xf014
```

每步都验证过：

| 面板值 | 含义 |
| --- | --- |
| `gcc=f8222000` | GDSC 上电（bit31=1, SW_COLLAPSE=0） |
| `clk=00000000` | 五个分支时钟全部运行（掩码为 0） |
| `smmu=00000001` | SMMU guard 通过（stream id 0x740） |
| `dwc=5533300a` | **DWC3 GSNPSID = v3.30a，控制器应答** |
| `step=7` | 进入 gadget start |

gadget 内部又加了 70–76 检查点，硬件停在这里：

```
USB step=73 gcc=f8222000 clk=00102000 smmu=00000000 pll=00520004 dwc=5533300a
```

- `clk` 是 `GCTL`：`0x00102000`，bits 13:12 = 2 → device mode ✓
- `smmu` 是 `DCTL`：`0x00000000` → 控制器 halt
- `pll` 是 `DSTS`：`0x00520004` → `DEVCTRLHLT=1`

step 73 = 软复位完成 + UTMI 时钟选择 + 事件缓冲就绪；卡在随后的 **EP0 命令序列**
（`DEPSTARTCFG` / `SETEPCONFIG` / `SETTRANSFRESOURCE` 之一返回失败）。

**下一个必需件是 QUSB2 PHY（`0x88e2000`）**：PHY 不上电就没有 PIPE 时钟，DWC3 的端点命令
永远等不到完成。需要按 `phy-qcom-qusb2.c` 的序列做：AHB2PHY 时钟与 QUSB2PHY 复位（GCC BCR）、
PHY 的 powerdown/retention 释放、PLL lock 轮询，再回来重放 EP0 命令。

注意：PHY 的 AHB 时钟若没开就去读写 `0x88e2000` 同样会挂总线，所以要先确认
`GCC_USB_PHY_CFG_AHB2PHY_CLK`（以及 `GCC_QUSB2PHY_PRIM_BCR`）的开法，再动 PHY 寄存器。

### QUSB2 PHY 已接入，PLL 不锁（2026-10-26 01:40）

`package/sirius_usb.c` 现在在 DWC3 之前做 PHY 上电：AHB2PHY 分支（GCC `0x6a004`）→
QUSB2PHY_PRIM 块复位（BCR `0x12000`，assert/100us/deassert）→ 16 条 v2 初始化表 →
释放 `POWER_DOWN`（`0x210`）→ 轮询 `PLL_LOCKED`（`0x1a0` bit5）。

面板结果：

```
USB step=68 gcc=F8222000 clk=22222222 smmu=00000001 pll=17171717 dwc=5533300a
```

- `pll=0x17171717`：bit5 = 0 → **PLL 未锁定**；
- `clk=0x22222222`（`0x210` POWERDOWN 读回）：不是写入的值（写入后应为 `0x22`），
  这种"每字节同值"的模式通常是**总线错误/未真正访问到寄存器**，而不是 PHY 内容；
- `gcc` 仍是 `f8222000`、`dwc=5533300a`：GDSC 与 DWC3 都正常，问题只在 PHY 访问/时钟。

下一步（按可能性排序）：

1. **只读 dump PHY 若干寄存器**（`0x04` 应回 `0x03`、`0x240` 应回 `0x30`），确认写入是否真的落到了 PHY；
   若读回仍是 `0x22222222` 一类，说明 `0x88e2000` 映射或 AHB 时钟不对。
2. 确认 SDM710 的 `GCC_USB_PHY_CFG_AHB2PHY_CLK` 偏移：`0x6a004` 来自 SDM845，需核对；
   可用只读方式扫 `0x6a000`–`0x6a020` 找带 halt 位的分支。
3. 若寄存器能读到但 PLL 不锁，检查 PHY 的参考时钟方案（`TCSR_PHY_CLK_SCHEME` 的 single-ended
   选择）与 CXO 是否到达 PHY；Linux v2 默认 `se_clk_scheme_default = true`。

已完成并可复用的部分（都不需要再试）：GDSC 上电、AXI/master/sleep/mock_utmi 分支、
SMMU guard（SID 0x740）、DWC3 识别与软复位。

### PHY 进展：可访问、可写、PLL 不锁（2026-10-26 02:05）

补上的两处（都是 Linux v2 序列里缺的）：

1. **PHY 参考时钟分支**：`gcc_usb3_prim_clkref_clk` 在 SDM845/SDM710 是 **`0x8c008`**
   （移植源自 SM7250 的 `0x8c010` 不是分支寄存器）。已按 halt 检查打开。
2. **PLL core reset 循环**：v2 配置 `has_pll_override = true`，必须先在
   `QUSB2PHY_PLL_CORE_INPUT_OVERRIDE(0xa8)` 置 `CORE_PLL_EN_FROM_RESET|CORE_RESET|CORE_RESET_MUX`
   （`0x70`），释放 POWER_DOWN 后清 `CORE_RESET|CORE_RESET_MUX`（`0x60`），PLL 才会出复位。

面板现状：

```
USB step=68 gcc=f8222000 clk=00000001 smmu=03030303 pll=17171717 dwc=5533300a
```

- `clk=1`：AHB2PHY 分支（`0x6a004`）已使能、halt 位为 0；
- `smmu=03030303`：写进 PHY `0x04` 的 `0x03` 能读回（寄存器可写；读回按字节复制是该 PHY 的行为）；
- `pll=17171717`：`PLL_STATUS(0x1a0)` bit5 = 0，**PLL 仍未锁定**。

已排除：AHB 时钟没开、寄存器写不进、缺 PLL core reset、缺 clkref 分支。
剩下的可能：

1. **参考时钟没真正到 PHY**：QUSB2 的 ref 来自 CXO，SDM670 的 DTS 没有 `tcsr` 节点，
   Linux 走 `se_clk_scheme_default = true`（单端）。若硬件实际是差分方案，PLL 不会锁，
   需要找 `TCSR_PHY_CLK_SCHEME` 的地址并读一次（叶子寄存器，只读安全）。
2. **PHY 供电（LDO）**：部分 SDM710 设计里 QUSB2 PHY 有独立 LDO，未上电则 PLL 不锁定，
   这需要 RPMH/regulator 路径。
3. `PLL_STATUS` 的偏移在 SDM710 上可能不同：可用只读方式扫 `0x1a0`–`0x1b0` 找 bit5 会变化
   的那个寄存器（PLL 释放复位后应有跳变）。

至此 USB 只剩 PHY 这一环；GDSC、AXI/master/sleep/mock_utmi 时钟、SMMU guard（SID 0x740）、
DWC3 识别与软复位、EP0 之前的全部步骤都已验证通过。

### USB 最后一环：EP0 不应答（2026-10-26 02:10）

进展到：**主机能看到设备**（`new high-speed USB device`），说明 PHY、DWC3、上拉、连接都对了。
但主机读不到描述符：

```
usb 1-2.2: device descriptor read/64, error -110
usb 1-2-port2: unable to enumerate USB device
```

面板原始事件（只保留最后两个事件字）：

```
USB step=76 gcc=00000201 clk=00000101 smmu=00000014 pll=00000000 dwc=00004181
```

解码：`0x101` = USB reset 设备事件，`0x201` = connect-done 设备事件，共 20 个事件；
`dwc=0x4181` = 16769 次内联 poll（轮询确实在跑）。**没有任何传输事件**，
即 EP0 的 SETUP TRB 从未被控制器消费。

已试过并排除：

- 内联轮询不足（改成 RUN 后连续 6 秒 1ms 粒度 poll，poll 计数证明在跑）；
- connect-done 之后没有重新 arm setup TRB（已加 `ud_start_setup()` + `DALEPENA=0x3`，仍无传输事件）；
- PHY 未就绪（先修好了：`0x1a4` bit0 core-ready / `0x1ac` bit5 lock）。

下一批要查的（按可能性）：

1. **`ud_epcmd` 的 DEPCMD 参数**：`DEPSTARTCFG`/`SETEPCONFIG` 的 param0/param1 位域是否与
   DWC3 3.30a 一致（尤其 `SETTRANSFRESOURCE` 的 fifo 号、`XferComplete` 使能位）；
   建议把 `DEPCMD(0)` 与 `DEPCMDPAR0/1/2(0)` 的读回值打到面板，确认命令真的被接受；
2. **setup TRB 的地址/属性**：`TRB_HWO|LST|ISP_IMI|IOC` 的组合，以及 `TRBCTL_SETUP` 是否要求
   只有 HWO+IOC（部分 DWC3 版本不接受 ISP/IMI 用于 control setup）；
3. **DCFG 的速度位**：connect-done 里没按 `speed` 设置 `DCFG.Speed`（目前只清了低 7 位），
   高/全速不匹配会让 EP0 包长错误；
4. `GCTL` 的 `PrtCapDir`/`DisScramble` 等位是否与 UTMI 模式匹配。

其中 1 和 2 最可能。建议下一次改动：在 connect-done 后把 `DEPCMD(0)`、`DEPCMDPAR0(0)`、
`DSTS`、`DCTL` 打到面板，并尝试用"仅 HWO+IOC"的 TRB 属性重新 arm 一次。

### USB 枚举成功（2026-10-26 02:40）

**设备已经能被主机正确识别**：

```
usb 1-2.2: Product: sirius XNU USB console
usb 1-2.2: Manufacturer: sirius
usb 1-2.2: SerialNumber: SIRIUS01
cdc_acm 1-2.2:1.0: ttyACM0: USB ACM device
```

也就是说：PHY → DWC3 → 上拉 → 控制传输（描述符/配置）→ CDC 接口绑定，**整条 USB 设备链路已经通了**。
`/dev/ttyACM0` 会出现，主机侧脚本 `scripts/capture-usb-console.py` 能正常打开。

**但 bulk IN 还没有数据**（抓取 0 字节）。已修的一处真 bug：`ud_ep0_config()` 早先只写
`DALEPENA = 0x3`（仅 EP0），把 RX/TX/NOTIFY 全关了；现在重置/连接后会重新配置并使能
全部端点（`0x3 | (1<<EP_RX) | (1<<EP_TX) | (1<<EP_NOTIFY)`）。修完仍然 0 字节。

剩下的排查方向：

1. `ud_tx_pump` 的触发：内联轮询窗口只有 10 秒（窗口太长会把 CPU 独占、面板冻结——已确认是
   这个原因），主机打开端口可能在窗口之后；而内核线程在 `bsd_init` 同步执行时不一定被调度。
   建议：把 pump 挂到一个必然被调用的周期性路径上（例如 boot-progress 的刷新里），或者
   把窗口延长到"环形缓冲被读空为止"但用 `delay()` 而非忙等让出 CPU。
2. TX TRB 的完成事件：`tx_busy` 是否卡死（第一次 IN 没完成就再也不发）。需要把
   `tx_busy`/`chars` 打到面板确认（已加字段，但窗口内面板不刷新，需缩短窗口再看）。
3. 端点描述符与物理端点号是否一致（EP_TX=3 → 0x83）。

注意：内联轮询窗口会独占 CPU 并冻结面板，调试时应保持 ≤10 秒，或改用会睡眠的等待。

### USB 控制通道完成，数据通道仍未通（2026-10-26 02:55）

主机侧已经完全正常：

```
usb 1-2.2: Product: sirius XNU USB console
usb 1-2.2: Manufacturer: sirius
usb 1-2.2: SerialNumber: SIRIUS01
cdc_acm 1-2.2:1.0: ttyACM0: USB ACM device
```

并修掉三个真实 bug：

1. **端点地址与物理端点不一致**：描述符原本写 `0x81/0x01/0x82`，而驱动实际配置的是 EP3/EP2/EP5
   （`0x83/0x02/0x85`）。主机的 IN token 打在从未使能的端点上。已改正。
2. **高速 bulk 的 `wMaxPacketSize` 必须是 512**：原来写 64，主机会报
   `bulk endpoint 0x2 has invalid maxpacket 64` 并禁用该端点。已改为 512。
3. **`ud_ep0_config()` 曾把 `DALEPENA` 只写 `0x3`**（仅 EP0），重置后把 RX/TX/NOTIFY 全关掉。
   现在会重新配置并使能全部四个端点。

**仍未解决**：bulk IN 一个字节都没有。最后的可观测状态（本轮之前的镜像）：

```
USB step=76 gcc=00000001 clk=00000000 smmu=00000000 pll=00000000 dwc=0000592d
```

`gcc=1` = 已配置；`clk=0` = `tx_busy` 没卡死；`smmu=0` = `ud.chars`（**已发送**字节数）为 0
→ `ud_tx_pump` 里的 `pur_copy` 一直返回 0 → **环形缓冲是空的**。

环形缓冲为空的根因（已定位）：`_cnputc` 的第一行是

```c
if (boot_progress_active()) { boot_progress_putc(c); return; }
```

而 `boot-progress=2` 一直开着，所以每个字符都在这里返回，**永远走不到补丁里加的镜像钩子**。
已在 `package/BootProgress.c` 的 `boot_progress_putc()` 开头直接调用 `sirius_usb_log_char(c)`
（符号确认存在于内核：`_sirius_usb_log_char @ 0xfffffff0079d0464`），但最近这次刷机后仍未验证
面板上的 `chars` 是否变正——**下一次上机第一件事就是看 `smmu`（=`ud.chars`）**：

- 若 `smmu>0`：镜像生效，问题在 TX TRB（`ud_start_trb(EP_TX, …)` 的完成事件），
  检查 `TRBCTL_NORMAL` 参数、`UD_TX` 地址与 512 字节包长；
- 若 `smmu=0`：说明 `boot_progress_putc` 也不是面板文本的来源，需要改为在
  `serial_console.c` 的 `_cnputc` **最开头**（`boot_progress_active()` 判断之前）插入钩子——
  这是补丁 `patches/xnu/sirius-usb-console.patch` 的下一次修改点。

### 数据通道最后一条线索（2026-10-26 03:25）

主机侧端点描述符已经完全正确（`lsusb -v` 确认 `0x85` EP5 IN、`0x83` EP3 IN、`0x02` EP2 OUT，
512 字节，无 "invalid maxpacket" 警告），设备也报告已提交 `ud.chars = 0xee5`（3829 字节）。

但主机读不到任何字节：`cat /dev/ttyACM0` 和 `capture-usb-console.py` 都是 0。

关键推断：**`ud.chars` 只在提交 TRB 时累加，不代表传输成功**。`tx_busy` 会被完成事件清掉，
说明 TRB 确实完成了——但完成状态（成功 / NAK 后错误 / 短包）我们没有检查。所以现在的现象
与"每次 IN 都以错误状态完成"完全一致。

下一步（下一次上机）：

1. 在 `ud_event()` 的 `ep == EP_TX && type == 1` 分支里读取**事件的 status 字段**
   （`(ev >> 12) & 0xf`），把非 0（错误）与成功分开计数，打到面板上；
2. 检查 `SETEPCONFIG` 的 param0 位域是否与 DWC3 3.30a 一致：`bits[1:0]` 端点类型、
   `bits[5:3]` maxpacket、`bits[16]`/`bit17`（XferComplete/XferInProgress 使能）、
   以及 IN 端点是否需要 `bit[17]=FIFO number`；
3. 确认 `EP_TX` 的 `SETTRANSFRESOURCE`（fifo 号 1）与 `DALEPENA` 对应位；
4. 若都正确，用 QSCRATCH 的 `GUSB2PHYCFG`/`GCTL` 复核 UTMI 方向（IN 传输走 UTMI 上行）。

至此 USB 的**控制通道 100% 工作**（枚举、描述符、CDC 绑定、ttyACM 节点），只剩 bulk IN 的
一次成功传输。这个结论已足够支撑明天一次性定位。

### 回显设施已加，数据仍不动（2026-10-26 03:40）

按"心跳包"思路加了双向自检：`EP_RX`（bulk OUT，`0x02`）始终挂着一个 512 字节的接收 TRB，
收到数据后立刻在同一字节数上从 `EP_TX`（`0x83`）回送，回显次数显示在面板 `pll` 字段。
`ud_poll()` 现在还从 boot-progress 的 `refresh()` 里驱动（那是唯一确定会被调度的内核路径）。

实测：主机 `printf "HELLO-ECHO-123\n" > /dev/ttyACM0` 后，回读仍为 0 字节。

**结论性判断**：两个方向都不动字节，且 `DALEPENA`/描述符/端点地址都已正确，最可能的原因
下沉到"端点命令没有被控制器接受"：

1. `ud_epcmd()` 的返回值从未检查——`DEPCMD(n)` 的 `CMDACT` 位与完成状态没有验证，
   `SETEPCONFIG`/`SETTRANSFRESOURCE`/`STARTTRANSFER` 可能全部静默失败；
2. `DEPCMD` 的 param0/param1 位域需按 DWC3 3.30a 复核（端点类型、maxpacket、fifo 号、
   `XferComplete` 使能）；
3. 建议下一次上机：把 `DEPCMD(EP_TX)`、`DEPCMD(EP_RX)`、`DEPCMDPAR0/1(EP_TX)` 的读回值
   直接打到面板，先确认命令被接受，再谈数据。

补充：`GCTL=0x00102000`（device mode）、`DCTL=0x80000000`（RUN）、`DSTS` 有连接状态、
事件环正常（reset/connect 事件都能收到并处理），说明控制器本身工作正常，问题局限在
端点命令/传输层。

### ✅ USB CDC-ACM 内核控制台打通（2026-10-26 15:45）

**主机第一次收到了内核启动日志，不再需要拍屏幕：**

```
$ sudo python3 scripts/capture-usb-console.py --wait 170 --duration 160 --output /tmp/epnum.log
captured 3813 bytes
```

日志开头就是真正的内核输出：

```
Darwin Kernel Version 23.6.0: Sun Oct 25 15:46:38 UTC 2026; u57u:ios-obj/DEVELOPMENT_ARM64_BCM2837
pmap_startup() init/release time: 6170 microsec
vm_page_bootstrap: 47443 free pages, 18093 wired pages
...
lockdown_mode: lockdown mode in nvram is off
```

### 根因：USB 端点号 ≠ 物理端点号

DWC3 的物理端点号是 `phys = 2 × USB端点号 + 方向`：

| 描述符里应写 | 物理端点 | 驱动配置的常量 |
| --- | --- | --- |
| `0x01` EP1 OUT | 2 | `EP_RX` |
| `0x81` EP1 IN | 3 | `EP_TX` |
| `0x82` EP2 IN | 5 | `EP_NOTIFY` |

之前为了"让描述符和物理端点一致"把它改成了 `0x83/0x02/0x85`，那对应物理端点 7/4/11 ——
**这块核上不存在**，所以主机的 IN token 全部落空：`DALEPENA=0x2F` 正常、`DEPCMD` 返回成功、
TRB 地址正确，但**永远没有传输完成事件**（这正是我们看到的 `tx_event=0`）。

改回 `0x81/0x01/0x82` 后立刻通。顺带确认了这条链上其它部分是好的：PHY、时钟、SMMU、
FIFO RAM（`TXFIFO0/1/2`、`RXFIFO0` 都已由启动链分配）、EP0 重配置、控制台镜像。

### 经验

- 面板 `tx_event`/`rx_event`（事件字）比任何推断都直接：`0` 就是"该端点从未完成过传输"，
  与"命令被接受、地址正确"同时出现时，几乎一定是端点号/映射错，而不是时钟或 DMA 问题；
- 调试期间加的 **echo 通道**（往端口写、设备原样回送）很有用，它把问题限制在"设备→主机"这一侧。

### 证据文件

`docs/evidence/sirius-usb-console-20261026-1605.log` —— 从 `/dev/ttyACM0` 直接抓下来的内核日志
（3813 字节），不再依赖拍照。复现命令：

```sh
# 主机侧先起抓取，再重启手机
sudo python3 scripts/capture-usb-console.py --wait 60 --duration 190 --output /tmp/boot.log
```

### 仍可改进（不影响"能用"）

- 现在每次连接只能拿到**连接时刻**环形缓冲里的内容（3813 字节），之后的实时输出依赖
  `sirius_usb_poll()`（挂在 boot-progress 刷新上）。若要让整个启动过程持续流式输出，
  把 `ud_poll()` 再挂一个更早、更密的内核周期路径（例如 boot-progress 的每秒刷新已有，
  但要确认它在 bsd_init 阶段确实被调用）。
- `capture-usb-console.py` 建议增加 `--follow`（持续追加写入）以便长时间抓取。

### 收尾：已验证的配置（2026-10-26 16:40）

连续两次刷机都稳定拿到 **3813 字节**内核日志。能工作的形态就是当前代码：

- 内联轮询循环 **45 秒**，且**在环形缓冲排空后立刻 break**；
- 首次推流延迟 3000 ms（在内联循环里按毫秒递减）。

试过但**不工作**（已回退，结论记录在此，避免以后重复踩）：

| 尝试 | 结果 |
| --- | --- |
| 内联循环 120 秒、不 break | 0 字节 |
| 内联循环 8 秒、不 break | 0 字节 |
| 用 `SET_CONTROL_LINE_STATE`(DTR) 门控 pump | 0 字节（主机的 DTR 没到达或不触发） |

也就是说：**prefetch 在循环内推、break 后交给周期路径**这个组合是有效的；把循环拉长或改变门控
条件反而什么都不发，机理还没完全搞清楚（事件通道、pump 状态机和循环时序之间似乎有耦合）。
在弄清之前不要动这段时序。

现状：每次连接能拿到"连接时刻环形缓冲的内容"（3.8 KB，覆盖到 `lockdown_mode`）。
要让整个启动过程持续流式输出，下一步应把 `ud_poll()` 挂到一个**确定在 bsd_init 期间运行**的
周期路径上（面板刷新已经是了，但需要验证它在 bsd_init 阶段确实被调用），而不是靠内联忙等。

## eMMC（SDCC1）第一阶段：控制器可用、卡已应答（2026-10-26）

### 已经打通的

板子是 **eMMC**，控制器是 SDCC1 `0x7c4000`（SDHCI 3.0，`ver=6c02`），
CQHCI `0x7c5000`、ICE `0x7c8000`，中断 GIC SPI 641/644，SMMU SID `0x140`。

探针 `package/sirius_emmc.c`（`scripts/build-sirius-emmc.py` 编译进
`libShengPlatformPower.a`，由 `patches/xnu/sirius-usb-console.patch` 在 `bsd_init` 里调用）读到：

```
EMMC probe step=2 ver=6c02 caps=762dc8b2 state=01f800f0 clock=0407 power=0d halts=0
```

- `ver=6c02`：SDHCI 3.0（高通厂商位 0x6c）；
- `caps=762dc8b2`：base clock 200 MHz，支持 3.3V/1.8V；
- `state=01f800f0`：DAT 线电平正常（注意 **bit16 CARD_INSERTED 不接**，eMMC 是 non-removable，不能拿它判断有无卡）；
- `clock=0407`、`power=0d`：卡时钟和总线电源都已由 bootloader 打开；
- `halts=0`：`gcc_sdcc1_apps/ahb/ice`、`aggre_ufs_phy_axi` 四个分支都在运行。

### 关键发现：卡时钟的 RCG 是无效配置

```
EMMC rcg cfg=00000401 m=00000000 n=00000000 d=000000ff
```

`sdcc1_apps_clk_src`（`cmd_rcgr 0x26028`）处于 **MND 模式但 M=0、N=0、D=0xff**——分频参数是垃圾值，
所以卡实际收不到有效时钟，**所有命令都超时、响应读回 0xffffffff（总线浮高）**。

按 Linux `ftbl_gcc_sdcc1_apps_clk_src` 的 400 kHz 条目（`BI_TCXO 19.2MHz / 12 × 1/4`）重写
MND（M=1, N=4, D=12）并触发 update 位之后：

```
EMMC set400k=ok cfg=00000000 m=00000001 n=00000004 d=0000000c
EMMC init ocr=40ff8080 ready=yes ...
```

**卡应答了**：OCR = `0x40ff8080`（bit30=1 → 高容量/按块寻址，电压窗口正常），`ready=yes`。

### 目前卡住的一步

`CMD2 (ALL_SEND_CID)` 与 `CMD3 (SEND_RCA)` 都没有响应：

```
EMMC cid=no rca FAIL int=8000 err=7fff st=01f800f1
EMMC resp=00000000 00000000 80000000 0040ff80    ← 仍是 CMD1 的 R3 响应
```

`st` 的 bit0（CMD_INHIBIT）在失败后保持 1，说明命令挂在总线上没完成；RESPONSE 寄存器仍是
上一条 CMD1 的内容，证明 **卡根本没回 CMD2/CMD3**，而不是解析错误。

下一步要查的（按可能性）：

1. **DAT0 busy**：eMMC 在 CMD1 之后可能仍用 DAT0 拉低表示忙，需要在发 CMD2 前等 DAT0 变高
   （`PRESENT_STATE` bits[23:20]）；
2. **CMD1 参数**：确认 OCR 里 sector-mode(bit30) 与电压位和这颗卡匹配；也可以再发一次 CMD1
   直到 `ready` 稳定；
3. **命令间延时/时钟**：识别阶段在每次命令间留出至少 74 个时钟，可在 `sdhci_command` 之后
   加一段自旋延时；
4. 若仍不行，考虑用 **CMD10(SEND_CID)** 或直接走 `CMD3 → CMD7 → CMD16 → CMD17`，
   跳过 CID（CID 只是信息性的）。

### 调试设施（可复用）

- 驱动的所有报告都通过 `sirius_usb_log_char()` 直接写进 **USB 环形缓冲**，因此刷机后主机
  `capture-usb-console.py` 就能读到——因为内核控制台在 `lockdown_mode` 之后就不再进环了。
- 面板新增一行：`EMMC step= ver= caps= st= clk= pwr= h=`。

### eMMC 第二阶段：卡应答 CMD1，卡在 CMD2（2026-10-26 20:10）

**已确认的事实**（全部来自 USB 控制台直接抓取，不是拍照）：

```
EMMC rcg cfg=00000401 m=00000000 n=00000000 d=000000ff      ← 修复前：MND 计数无效
EMMC set400k=ok cfg=00000000 m=00000001 n=00000004 d=0000000c ← 写 400 kHz 后
EMMC resp-before=00000000 00000000 00000000 00000000        ← 无陈旧响应
EMMC resp-after-cmd1=40ff8080 00000000 00000000 00000000    ← CMD1 是**真实**响应
EMMC init ocr=40ff8080 ready=yes
EMMC cid=no  rca FAIL int=8000 err=7fff st=01f800f1
```

- **CMD0/CMD1 真的被执行了**（响应寄存器从全 0 变成 0x40ff8080）；
- OCR = `0x40ff8080`：bit30=1 → 高容量/按块寻址；电压窗口正常；`ready=yes`；
- **CMD2（ALL_SEND_CID，136 位响应）和 CMD3（SEND_RCA，48 位）都没有响应**，失败后
  `PRESENT_STATE` 的 CMD_INHIBIT 一直是 1，命令线卡住。

已试过并排除：

| 尝试 | 结果 |
| --- | --- |
| CMD2 去掉 index check（136 位响应没有 index 字段，Linux 只在 `MMC_RSP_OPCODE` 时置位） | 仍失败 |
| CMD2 去掉 CRC 检查 | 仍失败 |
| 不等错误汇总位、只等 CC | 仍失败（说明不是提前中止） |
| 命令线卡住时自动 `RESET_CMD/RESET_DATA` 后重试 | 仍失败 |
| 跳过 CMD2 直接 CMD3 | 仍失败 |

**结论性判断**：48 位的 CMD1 能正常收发，136 位的 CMD2 完全无响应，且随后命令线卡死。
这已经超出 SDHCI 标准寄存器层，指向 Qualcomm vendor 块（`CORE_VENDOR_SPEC` 读回 0，说明
`0x10c` 这个偏移在 v5 核上不对）里的**响应采样/DLL 配置**——长响应需要 DLL 正确锁定。

下一步（下一次上机）：

1. 从 Linux `sdhci-msm.c` 里找 v5 核的 vendor 寄存器偏移（`CORE_VERSION` 的 major/minor 决定
   偏移表），读回 DLL 配置寄存器，确认 DLL 是否锁定；
2. 或试 **CMD10 (SEND_CID, addressed)** / 直接 `CMD7(RCA=1) → CMD13 → CMD17`，
   绕过 136 位响应（CID 只是信息性的，读块只需要 RCA + SELECT + blocklen）；
3. 若 DLL 没锁，需要在 vendor 块里按 `sdhci_msm_dll_init()` 的序列初始化 DLL 后再试。

### eMMC 第三阶段：命令通到 CMD1，数据阶段仍未通（2026-10-26 21:00）

**必须澄清的一点**：早先看到的 `sig=EFI ...` / `words=45464920` 很可能**不是我们读出来的数据**，
而是 bootloader 读 GPT 时残留在控制器 FIFO 里的最后一个字。证据：把 FIFO 连续读 4 次，四次
返回同一个值 `20494645`（"EFI "），说明读指针根本没有前进。

因此现在的准确结论是：**数据阶段（DATA phase）还没有成功过一次**。

已经确认可用的（都是硬证据）：

| 项目 | 状态 |
| --- | --- |
| SDCC1 寄存器 | `ver=6c02`、`caps=762dc8b2`、`corever=1000006c`（v1.108）、`vspec=0xa9c`（上电默认） |
| 时钟 | `gcc_sdcc1_apps_clk_src` 原本 MND 无效，改写成 400 kHz 后 `cfg=0/m=1/n=4/d=12` |
| 命令阶段 | CMD0/CMD1 真实执行，`resp-after-cmd1=40ff8080`、`ready=yes` |
| 卡状态 | OCR `0x40ff8080`（高容量、电压窗口正常） |
| 电源/时钟控制 | `pwrctl=00000002`（BUS_ON 成功）、`clock=0407`、`power=0d` |

**仍未通的**：CMD2（136 位响应）无响应、命令线随后卡死；以及数据阶段。

试过并失败的（都在代码里留有注释）：

| 尝试 | 结果 |
| --- | --- |
| PIO 读块（按 `BUFFER_READ_ENABLE` 位） | FIFO 返回同一个陈旧字 128 次 |
| 把 `HOST_CONTROL` 的 DMA 选择从 ADMA2 改成 SDMA（PIO 路径） | 仍返回陈旧字 |
| 单块传输启用 block count | 无变化 |
| 整控制器 `RESET_ALL` 后自己重配时钟/电源/总线 | 数据变成全 0（卡状态被打乱） |
| **SDMA 读块**（`SDHCI_DMA_ADDRESS` = 页对齐静态缓冲的物理地址，transfer mode 置 DMA 位） | 数据全 0 |

**下一步的关键怀疑点**：

1. **apps SMMU 的 SID**：SDCC1 的 DMA 走 SMMU（DTS 里 `iommus = <&apps_smmu 0x140 0xf>`）。
   USB 那条线我们为 SID `0x740` 做过 guard，这里没有做——SDMA 的数据会被 SMMU 拦掉，
   表现正是"传输完成但内存全 0"。下一次应当复用 `sirius_usb_smmu_guard()` 的思路给
   **SID 0x140** 建立同样的旁路映射，再跑 SDMA。
2. **PIO 的 FIFO 不前进**：可能是 `CORE_VENDOR_SPEC3`（v5 偏移 `0x250`）里的
   "DMA/PIO 选择"或 vendor 的 `SDCC_MCI_...` 配置仍在 DMA 模式；或需要 `HOST_CONTROL`
   的 DMA 选择真正切到 SDMA 且 `CORE_VENDOR_SPEC` 的对应位一起改。
3. **CMD2 的 136 位响应**：DLL 处于掉电复位（`dll=6000642c`、`dllst=0` bit7=0），
   长响应可能要求 DLL 锁定；按 `sdhci_msm_dll_init()` 初始化后再试。

**建议的下一步顺序**：先做 SID 0x140 的 SMMU 旁路 + SDMA 读（最可能直接拿到 GPT），
再做 DLL 与 CMD2（为了完整识别），最后才谈块设备/I-O 层。

### eMMC 第四阶段：命令在 CMD1 之后失效（2026-10-26 21:15）

本轮又排除了两条：

| 尝试 | 结果 |
| --- | --- |
| `CMD7 (SELECT_CARD, RCA=1)` 之后 CMD17（假设 bootloader 用 RCA=1 选中过） | `cmd7(rca1)=fail`，随后读块仍失败 |
| SDMA 读（页对齐缓冲 + `SDHCI_DMA_ADDRESS`） | 数据全 0；`xfer` 读回 `0x33`（bit5 = auto-CMD12 仍被置位），说明转移模式寄存器还带着启动链的位 |
| 直接 CMD17（不做任何初始化，靠 bootloader 留下的是选中状态） | 失败 |

**现在的核心谜团**：`CMD0 + CMD1` 能得到真实的 OCR（`resp-after-cmd1=40ff8080`、`ready=yes`），
但**之后的每一条命令（CMD2/CMD3/CMD7/CMD13/CMD17）都没有响应**，并且命令线在第一次失败后
就保持在 CMD_INHIBIT。

一个值得注意的细节：`xfer` 读回 `0x33` 而不是我们写进去的 `0x13`——bit5（Auto CMD12 使能）
是我们没写却被置位的。**这说明 `SDHCI_TRANSFER_MODE` 的写入没有完全生效**，或者控制器在
vendor 层被固定成某种模式。下一步应当验证写入是否真的落到寄存器（写一个特征值再读回），
这能一次判定"寄存器写入被忽略"还是"命令没发出去"。

另外：`SDHCI_TRANSFER_MODE` 是 **16 位寄存器**，偏移 `0x0c`；`SDHCI_COMMAND` 是 16 位、偏移
`0x0e`。若用 32 位访问会同时改写两个寄存器，务必保持 16 位读写（当前代码是 16 位）。

**建议下一次的顺序**：

1. 写 `TRANSFER_MODE = 0x1234` 再读回，确认寄存器写入是否生效（判定 vendor 只读/锁定）；
2. 若写入无效，检查 `CORE_VENDOR_SPEC3`（v5 `0x250`）与 `CORE_VENDOR_SPEC` 是否把控制器
   锁进某种自动模式；
3. 若写入有效，用 `CMD13 (RCA=1)` 的响应判断卡状态，再决定是 CMD7 还是重新 CMD3 取 RCA。

### 给下一次的一句话总结

SDCC1 的寄存器层、时钟（400 kHz MND）、电源与 CMD0/CMD1 都已经用硬证据确认可用；
剩下的全部问题集中在 **CMD1 之后的命令不再被执行** 和 **数据阶段从未真正传输过**
（之前看到的 `EFI ` 是 bootloader 留在 FIFO 里的陈旧字，不能用它判断成功）。

### ⚠️ 危险记录：不要盲写 SMMU（2026-10-26 21:30）

本轮为了给 SDCC1 的 DMA 建旁路，直接向 apps SMMU 写了 SMR/S2CR（SID `0x140`）。结果
**整机在启动过程中失去 USB、无任何输出**，需要强制重启才能恢复。

**结论**：SMMU 在运行中（已使能、已有 context bank 在用）时，从内核里盲写 SMR/S2CR 是**危险操作**，
可能挂死整个 SoC。USB 那条线之所以安全，是因为它只**读取并校验**已有映射，从不新建。

以后如果要给存储流建映射，正确做法是：

1. 先只读确认 bootloader 留下的映射（本轮已改成只读探针 `emmc_smmu_probe()`）；
2. 若要新建，必须按 context bank 的正确流程（CBAR/CBA2R/SCTLR + 页表）做，并且**在 SMMU 关闭
   或该 stream 未启用时**写入；
3. 更安全的替代：**不使用 DMA**，走 PIO（但 PIO 也有问题，见下），或用 SMMU 之外的单层映射。

### 数据阶段的真实状态（本轮最重要的发现）

```
EMMC cmd17 ok, no TC int=0000 err=0000 st=01f802f6
```

- **CMD17 命令被卡接受**（`cmd17 ok`）；
- **没有 Transfer Complete**，`INT_STATUS` 全 0；
- `PRESENT_STATE` 的 **bit2 = DAT Line Active** → 数据阶段**已开始但永不结束**。

这排除了"卡不在状态"的假设：命令合法、卡接受了，数据阶段卡在传输中。两个候选原因：
**SMMU 拦截 DMA**（本轮尝试修，但写挂了）或 **DLL/采样**导致数据无法完成。

### 下一步（安全路径）

1. 先用只读探针看 SID `0x140` 的既有映射（本轮镜像已改为只读）；
2. 若没有映射 → 用 **PIO**（`TRANSFER_MODE` 不带 DMA 位）重试，并确认 `TRANSFER_MODE`
   写入在命令线干净时确实生效（本轮验证：干净时能写 `0x13`，卡住时写入被忽略）；
3. 若 PIO 仍卡在 DAT Line Active → 问题在数据采样（DLL），按 `sdhci_msm_dll_init()` 初始化。

### 数据阶段死因已确认：SMMU 对存储流是 abort（2026-10-26 21:45）

只读探针读出：

```
EMMC smmu140=00000000     →  slot 0, bank 0, S2CR type = 0 = ABORT
```

ARM SMMU 的 `S2CR` 里 **type 0 = abort**。也就是说 bootloader 交给内核时，**存储流（SID 0x140）
被配置成"拒绝所有访问"**。这与观察完全吻合：

```
EMMC cmd17 ok, no TC int=0000 err=0000 st=01f802f6   ← DAT Line Active 永远不结束
```

- CMD17 被卡接受；
- 数据引擎把数据往内存写时被 SMMU 终止 → 传输永远不完成；
- PIO 也不通（FIFO 不前进，读回同一个陈旧字 `20494645`），因为控制器在 DMA 模式下
  并不把数据送进 CPU 可读的 FIFO。

### 下一次的正确修法（顺序至关重要）

上一轮盲写导致整机挂死，原因就是顺序错了：先写 SMR（立即生效、指向仍在 abort 的 S2CR），
在写 S2CR 之前的窗口里，该流的所有在途访问都被 abort → SoC 挂死。

正确顺序：

1. **先写 S2CR**（该 slot）= `1`（bypass），使该流立刻不再被拒绝；
2. `dsb sy`；
3. **再写 SMR** = `(1 << 31) | 0x140`（VALID + 精确匹配）；
4. 再次 `dsb sy`，并且**在写之前确保没有该流的传输在途**（命令线空闲、无 DAT active）。

若再挂，就换用 context-bank 正规路径（CBAR/CBA2R/SCTLR + 页表），不要直接改 S2CR。

### SMMU bypass 已安全应用，但数据阶段仍不完成（2026-10-26 21:55）

按"最小改动"思路，只把匹配 `0x140` 那个 SMR 的 **S2CR 从 0（abort）改成 1（bypass）**，
不写 SMR、不新建 slot。结果：

```
EMMC smmu140=00000000 fixed=1 cmd17 ok, no TC int=0000 err=0000 st=01f802f6
```

- `fixed=1`：写入生效并读回确认，**整机没有挂**（证明之前的挂死确实来自"SMR 先于 S2CR"的顺序错误）；
- 但 **CMD17 仍然没有 Transfer Complete**，`DAT Line Active` 依旧保持，PIO 也依旧返回陈旧字。

**因此 SMMU 不是（唯一的）原因。** 现在还剩下的候选：

1. **数据采样 / DLL**：`dll=6000642c`（PDN+RST 置位）、`dllst=0`（bit7 未锁定）。SDCC v1.108 在
   默认速度下通常旁路 DLL，但若启动链把它关掉而控制器又依赖它采样，数据阶段就会挂住。
   下一步应实现 `sdhci_msm_dll_init()` 的序列（`CORE_DLL_CONFIG` 0x200 / `CORE_DLL_STATUS` 0x208
   / `CORE_DLL_CONFIG_2` 0x254 / `CORE_DLL_USR_CTL` 0x388），把 DLL 拉起来后再试。
2. **卡的 8 位总线与 CMD6(BUS_WIDTH)**：`hostctl=38` 说明控制器是 8 位；如果卡其实还在 1 位
   （bootloader 用 CMD6 设过、但被我们后续的 CMD0/CMD1 复位清掉了），数据相位会对不上。
   可以先用 1 位（清 hostctl bit5）再试一次，成本很低。
3. `CORE_VENDOR_SPEC3=02626440` 里的位含义未知，需要对照 `sdhci-msm.c` 的 v5 分支解读。

**给下一次的建议顺序**：先试"1 位总线"（改一行），再试 DLL 初始化，最后才回到 SMMU/寄存器细节。

### ⚠️ 探针必须在启动早期跑完，且不能卡（2026-10-26 22:00）

eMMC 探针在 `bsd_init` 里、**USB 控制台启动之前**运行（因为控制台只把"连接时刻"的环形缓冲
交给主机，探针输出必须在那之前写进环）。这带来一个硬约束：

> **探针里任何一次卡死的等待，都会让内核走不到 USB 控制台，从而失去唯一的日志通道。**

本轮把 `HOST_CONTROL` 改成 1 位总线后整机就没再枚举（`device not accepting address, error -62`），
只能靠强制重启恢复。已回退。

**以后写探针的规则**：

1. 每一次等待都要有很短的硬上限（建议 ≤200 ms），超时就报告并返回，绝不无限等；
2. 一次刷机只改**一个**变量，改完先确认能开机、能出日志；
3. 任何写 SMMU / 改总线宽度 / 改时钟的改动，都要先把"能启动"作为前置条件验证。

### 数据阶段：三种 DMA 模式全部失败，卡确实在 TRAN 且无错误（2026-10-26 22:20）

本轮把数据路径的三种模式都试过了，结果完全一致：

| 模式 | 结果 |
| --- | --- |
| PIO（`TRANSFER_MODE=0x12`） | FIFO 返回同一个陈旧字 128 次（`20494645` = "EFI "） |
| SDMA（`0x13`，`DMA_ADDRESS`=页对齐缓冲） | `cmd17 ok, no TC`，`INT_STATUS=0`、`ERR_INT_STATUS=0` |
| **ADMA2 64 位**（`hostctl` bits[4:3]=0b11，12 字节描述符 `cmd=0x23,len=512`） | 同样是 `adma2 no TC int=0000 err=0000` |

同时拿到的**决定性事实**：

```
EMMC r1=00000900
```

R1 解码：`CURRENT_STATE`(bits[12:9]) = **4 = TRAN**，`READY_FOR_DATA`(bit8) = 1，**没有任何错误位**。
卡片处于传输态、准备好数据、接受了 CMD17。

配合 `PRESENT_STATE` 的 DAT 线电平（bits[23:20] = `0xF`，全高）与 `gfsr=00000000`（SMMU 无故障）：

> **命令通道正常、SMMU 已放行、卡在 TRAN 且无错误，但 DAT 线上没有任何数据活动。**

这把问题范围压缩到**数据线本身**（DAT 焊盘/供电/复用，或卡的 DAT 输出未使能），而不是寄存器配置。

下一次（按成本排序）：

1. 采样 `PRESENT_STATE` 的 DAT 位在传输期间是否翻转（本轮已加入 `dat seen=/trans=` 报告，
   但抓取窗口错过，需下一次上机确认）——**这一个数据点就能区分"卡不驱动"还是"采样不对"**；
2. 若 DAT 完全不翻转 → 查 `CORE_VENDOR_SPEC` 的 `CORE_IO_PAD_PWR_SWITCH(_EN)` 与 SDC1 数据焊盘
   的供电/复用（对照 `sdm670.dtsi` 的 `sdc1_data` 与 TLMM 配置）；
3. 若 DAT 有翻转但无 TC → 是采样问题，按 `sdhci_msm_dll_init()` 拉 DLL 并配相位。

### 抓取的时序问题（工具改进）

环形缓冲**只在 USB 枚举时发一次**，主机若在那之后才打开串口就什么都拿不到（本轮多次踩到）。
下次应给驱动加一个"收到主机任意字节就重发整个环"的钩子，或让 `capture-usb-console.py`
在刷机**之前**就持续轮询端口（本轮的 `/tmp/grab.sh` 思路正确，但要把轮询窗口放到刷机前）。

### 参考模块：QcomXnuUsbDebug（2026-10-26 22:45）

`https://github.com/xiaoleGun/QcomXnuUsbDebug` 是一个面向 SM8550 的 XNU USB 调试 kext，
和我们自建的 CDC-ACM 控制台解决同一类问题，但工程化程度高得多。值得借鉴的三点：

1. **SMMU 的正确用法**：它不用 bypass，而是给 USB 流建**私有 S1 转换域**（3 页页表、只映射
   8 页自有 DMA、叶子不可执行），并明确写明"BYPass 只能当诊断，不能作为降级方案，不能关闭
   全局 SMMU 或改动其他设备的 stream"。我们目前对存储流用的 bypass 也应视为临时手段。
2. **提交顺序**：`S2CR`(S2PREP) → `SMR`(SMRPREP) → 读回校验 → `COMMIT`（最后一次写才让流生效），
   每步都有 ownership 检查。这正是我们上次挂死缺少的东西。
3. **连续日志 + 按需控制**：主机通过控制 socket 随时取日志、重启、截图，而不是只在枚举时给一次快照。

**我们这边已经落地的等价改进**：驱动在收到主机任意字节时把环缓冲读位置清零（`ud.tx_pos = 0`），
`capture-usb-console.py` 打开串口后主动发一个换行。实测可以按需取到启动后产生的日志
（例如 panic 文本），不再受"枚举瞬间"限制。

**尚未做的**：完整移植该 kext 需要适配 SoC（我们 SDM670 的 GCC/DWC3/SMMU 地址与 SID `0x740`
都不同）和 XNU 版本（它对应 Darwin22 / XNU 8796.141.3），工作量大于当前收益；优先借用它的
SMMU 提交顺序与日志轮询思路。

## eMMC 第五阶段：对照 U-Boot 逐项排除（2026-10-27 00:50）

这一轮按用户建议，直接对照 **U-Boot 的 `drivers/mmc/msm_sdhci.c` 与 `drivers/mmc/sdhci.c`**
（这才是本机真正能读 eMMC 的实现），逐项排除了若干猜测：

| 项目 | U-Boot 的做法 | 我们的实测 | 结论 |
| --- | --- | --- | --- |
| DLL | `clock < 100MHz` 时**故意**置 `CORE_DLL_RST` + `CORE_DLL_PDN` | `dll=6000642c`（PDN+RST） | **一致，DLL 不是问题** |
| `CORE_VENDOR_SPEC` | 写 POR 值 `0xa9c` | `vspec=00000a9c` | **一致** |
| v5 核的 MCI 初始化 | `mci_removed=true` → 跳过 `SDCC_MCI_POWER`/`HC_MODE` | — | 不需要 |
| `TIMEOUT_CONTROL` | 每个数据命令前写 `0x0e` | `timeout-in=0e`（本来就是） | **一致，不是问题** |
| SDHCI 分频器 | MSM **不使用**内部整除（Linux 明确注释），速率取自 GCC | `clock=0407` 写不进去 | **正常**，卡时钟就是 GCC 的 400 kHz |
| 供电电压 | 由板级 regulator 决定 | **DTS: `vqmmc = vreg_l5b_1p8`（1.8V）** | 我们原本是 3.0V → 已改成 1.8V（`power=0b`、`hc2=0a`） |

### 当前最可疑的一条

e63 之后关掉了"先做一次直接读"的路径，结果 **CMD1 不再应答**：

```
EMMC resp-after-cmd1=ffffffff  ready=no
```

而只要先跑过数据命令（里面有约 2 秒自旋），CMD1 就能拿到真实的 `40ff8080`。
已尝试加 300 ms 延时 —— **无效**。所以不是简单的上电等待，更像是：

> 卡停留在 **TRAN** 态（R1 已证明），**CMD0 没有真正把它打回 idle**，于是 CMD1 被忽略；
> 之前那次"能应答"很可能是因为数据命令路径里的某些操作改变了状态。

下一次要验证的（按顺序）：

1. 报告 **CMD0 是否真的完成**（本轮已加 `cmd0=ok/fail` 打印，但抓取窗口错过）；
2. 在 CMD0 前先读一次 **CMD13 (RCA=1)** 确认卡状态；
3. 若 CMD0 确实发出但无效，试 **拉低再恢复 SD 时钟** 或 **整控制器 `RESET_ALL` 后重新初始化**
   （注意：`RESET_ALL` 会清 vendor 配置，之后必须重写 `CORE_VENDOR_SPEC=0xa9c`）。

### 抓取通道的遗留问题

启动时那个 45 秒内联轮询窗口过后，后台线程 `ud_thread` 似乎不再服务"主机请求重发"
（实测 poke 无响应）。下次应确认该线程是否真的在运行，或改为在 panic/中断里也能泵送。

## eMMC 第六阶段：子代理对照 U-Boot/Linux 找出四个确定性 bug（2026-10-27 01:30）

用两个子代理分别精读 U-Boot 与 Linux 的实现，逐行对照我们的驱动，找出并修掉了四个
**确定性错误**。这些错误解释了此前所有"无法解释"的现象：

### 1. 中断/错误寄存器映射是错的（最严重）

SDHCI 3.0 的实际布局：

| 偏移 | 寄存器 |
| --- | --- |
| `0x30` | NIS（bit15 = 错误汇总） |
| `0x32` | **EIS 错误状态** |
| `0x34` | NISE |
| `0x36` | EISE |
| `0x38` | **NSIGEN 中断信号使能** |
| `0x3a` | ESIGEN |
| `0x3c` | Auto-CMD12 错误状态 |

我们此前把 `0x38` 当成"错误状态"、`0x3c` 当成"错误使能"，于是：

- 日志里每一个 `err=7fff` 其实是**我们自己写进信号使能寄存器的 0xffff**；
- `0x32` 的真实错误状态**从未被读过一次**；
- EIS 从不清理，导致 NIS 的 bit15（错误汇总）永远置位，于是我们误判为"虚假错误位"。

修正后用 32 位访问 `0x30` 清除、`0x34` 使能、`0x38` 写 0。

### 2. ADMA2 描述符地址写错了寄存器

`0x00` 是 SDMA 系统地址，**ADMA2 描述符表地址在 `0x58`/`0x5c`**。我们一直写 `0x00`，
所以 **ADMA2 从来没有真正工作过**。修正后立刻拿到了真实错误：

```
EMMC adma2 adst=01 no TC int=8000 err=0200
```

- `int=8000`：NIS 错误汇总置位（此前被无视）；
- `err=0200`：EIS bit9 = **ADMA Error**；
- `adst=01`：ADMA Error State = **取描述符阶段失败（ST_FDS）**。

### 3. 时钟 RCG 编码错误

`CFG=0` 表示"整数分频模式"，会**旁路 M/N**，所以我们写的 `M=1 N=4 D=12` 根本没生效。
按 `clk-rcg2`/U-Boot 的编码方式应为：

```
CFG = 0x2017   (pre_div 23 | DUAL_EDGE | src 0 = BI_TCXO)
M   = 1
N   = 0xfc     (~(4-1)，硬件存储的是补码)
D   = 0xfb     (~4)
```

修正后 `cfg=00002017 m=00000001 n=000000fc d=000000fb`，卡时钟才真正是 400 kHz。

### 4. 从未发送 CMD7 (SELECT_CARD)

未选中的卡仍会应答 CMD13（所以"卡活着"的假象成立），但**只有被选中的卡才会执行 CMD17**。
已加入 CMD7(RCA=1, R1b)。当前 `cmd7=fail` 仍需排查（可能与我们传的响应类型或 RCA 有关）。

### 当前状态

```
EMMC rca=1 st=00000900 cur=4        ← 卡在 TRAN，无错误
EMMC cmd7=fail r1=00000000          ← CMD7 无响应（待查）
EMMC clock=0007                     ← PIO 路径把分频设为 ÷1，400 kHz
EMMC dat seen=801f trans=8          ← DAT 线上确实出现了数据（值 0..4）
EMMC adma2 adst=01 ... err=0200     ← 描述符取指失败
EMMC pio-distinct=1                 ← PIO 的 FIFO 始终返回同一个字
```

**结论**：卡在正常发数据（DAT 线有活动），瓶颈完全在主机侧取数：

- **ADMA** 卡在描述符取指（`ST_FDS`）——最可能是 **IOMMU 未映射我们的描述符页**；
- **PIO** 的缓冲端口始终返回同一个字（可能与 caps 里 SDMA 不可用、或需要不同的读取节奏有关）。

### 下一步（明确的两条路）

1. **给存储流建正规 S1 转换域**：参考 `/tmp/qcdebug/soc/QcomUsbDmaDomain.c`（QcomXnuUsbDebug
   模块里的完整实现：3 页页表、S2CR/SMR 提交顺序、TLBI/SYNC、ownership 校验），把我们的
   描述符页和 512 字节缓冲区映射进去。⚠️ 直接写 S2CR 已经两次把 SoC 写挂，必须按该模块的
   顺序做（S2CR 先、SMR 后、最后提交）。
2. **把 PIO 修好**：它是唯一不依赖 IOMMU 的路径。需要弄清为什么缓冲端口不前进——可以试
   每次 `BUFFER_READ_ENABLE` 读满 16 个字（一个 FIFO 突发），或改用 8/16 位访问试探。

**风险记录**：本轮又有两次因写 SMMU 导致整机挂死/重启循环，现已改为**只读**。

## 🎉 eMMC 打通：XNU 读出了 GPT 头（2026-10-27 02:10）

```
EMMC read=ok sig=EFI PART
EMMC after=ok st=00000900 cur=4
```

**存储数据通路终于打通**。LBA 1 的真实内容是 `EFI PART`，传输完成后卡仍处于 TRAN 状态且无错误。

### 最终凑齐的五个条件（缺一不可）

| # | 条件 | 之前的状态 | 修复 |
| --- | --- | --- | --- |
| 1 | 卡时钟 | RCG 写成 `CFG=0`，M/N 被旁路 | 按 clk-rcg2 编码：`CFG=0x2017`、`M=1`、`N=0xfc`、`D=0xfb` |
| 2 | I/O 电压 | `POWER_CONTROL=0x0d`（3.0V），而板子 vqmmc 是 1.8V | 改 `0x0b` + `HOST_CONTROL2` bit3 |
| 3 | 中断/错误映射 | `0x38` 当成错误状态（其实是信号使能） | 真正的 EIS 在 `0x32`，32 位访问 `0x30` 清除 |
| 4 | ADMA2 描述符地址 | 写到 `0x00`（SDMA 地址） | 改 `0x58`/`0x5c` |
| 5 | **Cache 一致性** | CPU 写描述符走可缓存映射，DMA 读到全 0 | **`flush_dcache64(物理地址, 长度, TRUE)`** |

第 5 条是最后的关键：没有它，ADMA 引擎读到的是旧内存（全 0），于是"看不到 VALID 位"，连续走过描述符
直到报 `adst=01`（取指错误）。加上 cache 维护后一次成功。

### 顺带修好的调试设施

- `capture-usb-console.py` 新增 `--match PATTERN`：匹配到就自动结束，不用等满时长；
- USB 驱动的工作线程**每秒把整个环形缓冲重发一次**，因此**任何时候连上串口都能拿到完整启动日志**
  —— 实测晚连 60 秒仍抓到 75973 字节，之前反复"抓取窗口错过"的问题彻底消失。

实测日志里可以看到好几分钟前的探针输出被完整重发，这对后续调试价值很大。
