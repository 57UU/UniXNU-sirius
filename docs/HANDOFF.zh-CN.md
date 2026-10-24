# UniXNU on Xiaomi Mi 8 SE (sirius) — 交接文档

更新：2026-10-24。本文档交接"把 UniXNU（XNU 内核 + iPadOS 用户态）移植到小米 8 SE"的全部上下文：当前进度、架构决策、构建/部署循环、已解决的问题、下一步路线。

---

## 1. 项目概况

| 项 | 内容 |
| --- | --- |
| 目标设备 | 小米 8 SE（sirius，Snapdragon 710 / Kryo 360，6GB eMMC 机型） |
| 目标系统 | [UniXNU](https://github.com/UniXNU/UniXNU)：XNU 23.6.0（4KiB BCM2837 personality）+ 预链接 iPad7,1 iPadOS 17.7.11 (21H461) 内核缓存 |
| 设备仓库 | https://github.com/57UU/UniXNU-sirius（本仓库；CI 在此构建内核） |
| 本机工作区 | WSL `~/Xnu4Sirius/`：`work/UniXNU`（主仓库检出）、`UniXNU-sirius`（本仓库）、`payload-full.img` 等产物 |
| 参考端口 | UniXNU-fajita（OnePlus 6T，SDM845 —— 最接近的同代 SoC）、UniXNU-sheng |
| 相关资产 | `~/sirius-distro`（手机上的 Linux 发行版，含 U-Boot 源码）、`~/rocky-sdm710-linux`（安卓内核）、`~/Xnu4Sirius/work/uefi-sheng`（Mu-Silicium，Font.h 来源） |

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

## 2. 当前状态（2026-10-24）

| 阶段 | 状态 | 证据 |
| --- | --- | --- |
| S1 UEFI（U-Boot） | ✅ | UniXNU 菜单/驱动加载在真机验证 |
| S3 payload 卷 | ✅ | p80 FAT + 标记自动引导 |
| S4 内核入口 | ✅ | 绿/蓝/品红探针色带 |
| S5 MMU/定时器/PSCI | ✅ | stage-3 复位循环（20s 定时 + PSCI 复位） |
| S6 控制台 | ✅ | boot-progress 面板：内核日志 + NVRAM 代理 + 19.2MHz 时基全通 |
| S7 IOKit | 🔶 进行中 | GIC 预配置后越过 lockdown 进入 kext 启动；AppleARMCPU 抢占 /cpus nub 的根因定位中（TOPO 诊断面板已部署） |
| S8 root device | ⏳ | 无存储驱动（eMMC 机型，无 UFS）；预期停在 root 等待/panic |

**最新部署**：TOPO 诊断版（无 ArmCpu 覆盖 + 面板 TOPO 行）。`sheng-smp=1` 参数没有防住抢占 —— 证据是面板 GIC stride=0 / SMP req=0（ArmCpu::start 从未运行），而 probe 逻辑与 fajita 相同；带 probe 计数器的 ArmCpu 覆盖版内核会**破坏内核**（死在面板之前，疑为对象布局平移），已回退。TOPO 行读取内核拓扑（ml_get_cpu_number × 8 个 MPIDR + ArmDTPlatform 计数器），数据到手后走 DarwinBoot 层验证 ADT/nub 属性。

## 3. 构建-部署-测试循环

### 3.1 内核（改 defconfig/kit/补丁时需要 —— macOS CI）

```bash
cd ~/Xnu4Sirius/UniXNU-sirius
git add -A && git commit && git push          # push 到 main 自动触发（注意 paths 过滤器）
# 若 paths 不覆盖（如 package/ 之外的杂项）手动触发：
gh workflow run kernel.yml --ref main -f steps=kernel
# 等待 ~8 分钟，下载产物：
gh run download <run-id> -D ~/Xnu4Sirius/kernel-artifact/tmp -n sirius-kernel-<N>
```

⚠️ gh run download 在本网络下经常 connection reset / 卡死。可靠方法（走 Windows 宿主代理，WSL 里宿主机 = 网关 172.23.0.1）：

```bash
export https_proxy=http://172.23.0.1:7900 http_proxy=http://172.23.0.1:7900
TOKEN=$(gh auth token)
# 1) 取 artifact id
curl -s -H "Authorization: Bearer $TOKEN" \
  "https://api.github.com/repos/57UU/UniXNU-sirius/actions/runs/<RUN>/artifacts"
# 2) 拿 302 的 Location（blob storage）
curl -s -I -H "Authorization: Bearer $TOKEN" \
  "https://api.github.com/repos/57UU/UniXNU-sirius/actions/artifacts/<ID>/zip"
# 3) wget Location（必须走代理），解压用 python3 -m zipfile（无 unzip）
```

⚠️ workflow 的 concurrency 是 `cancel-in-progress: true` —— 构建期间**不要 push**，会取消正在跑的构建（发生过两次事故）。

### 3.2 EFI 程序（本地 WSL，秒级）

```bash
cd ~/Xnu4Sirius/work/UniXNU
./unixnu build package        # UniXNU.efi（已构建过，一般不用重跑）

# DarwinBoot.efi（本仓库覆盖版，含 GIC 预配置）：
cd ~/Xnu4Sirius/UniXNU-sirius/drivers/DarwinBoot
WORKSPACE=$PWD \
PACKAGES_PATH=$PWD:$HOME/Xnu4Sirius/work/UniXNU/package:$HOME/Xnu4Sirius/work/UniXNU/out/feeds/mu_basecore-89034cbb \
EDK_TOOLS_PATH=$HOME/Xnu4Sirius/work/UniXNU/out/feeds/mu_basecore-89034cbb/BaseTools \
CONF_PATH=$HOME/Xnu4Sirius/work/UniXNU/out/sirius/package/Build/Conf \
CLANG_BIN=/usr/bin/ PYTHON_COMMAND=python3 \
PATH=$EDK_TOOLS_PATH/BinWrappers/PosixLike:$EDK_TOOLS_PATH/Source/C/bin:$PATH \
build -p DarwinBootPkg/DarwinBoot.dsc -a AARCH64 -b DEBUG -t CLANGPDB -n 32
# 产物: build/DarwinBoot/DEBUG_CLANGPDB/AARCH64/DarwinBoot.efi
```

### 3.3 payload 组装（本地，约 1 分钟）

```bash
cd ~/Xnu4Sirius/work/UniXNU
UNIXNU_UEFI=$HOME/Xnu4Sirius/work/uefi-sheng \
./unixnu build payload --kernel ~/Xnu4Sirius/kernel-artifact/kernel-bp.macho   # 或新内核
python3 ~/Xnu4Sirius/UniXNU-sirius/scripts/make-uboot-payload.py \
  --payload out/sirius/payload/payload.img \
  --unixnu-efi out/sirius/package/Build/Out/AARCH64-DEBUG/UniXNU.efi \
  --darwinboot-efi ~/Xnu4Sirius/UniXNU-sirius/drivers/DarwinBoot/build/DarwinBoot/DEBUG_CLANGPDB/AARCH64/DarwinBoot.efi \
  --marker --output ~/Xnu4Sirius/payload-full.img
```

- `--marker` 写入 BOOTUNIXNU（自动启动 UniXNU；去掉则开机默认 Linux）。
- `UNIXNU_UEFI` 提供 panic-screen 探针需要的 Font.h（uefi-sheng 检出）。

### 3.4 真机部署

```bash
# 手机侧进入 fastboot：UniXNU 菜单选 Power off → 音量下+电源；Linux 里 sudo reboot bootloader
# Windows 侧（Git Bash）：
fastboot.exe flash vendor "//wsl.localhost/Ubuntu-26.04/home/u57u/Xnu4Sirius/payload-full.img"
fastboot.exe reboot
```

恢复 Linux：fastboot 下 `fastboot flash boot ~/sirius-distro/build/boot.img`（原版 U-Boot，不认标记，直接进 Linux），操作完再把 `~/Xnu4Sirius/boot-unixnu.img` 刷回去。手机回到 Linux 后 SSH：`ssh u57u@172.16.42.1`（免密已铺）。

### 3.5 观察输出

屏幕即调试器：boot-progress 面板（CPU/SMP/CLK/MEM/GIC/DISPLAY 各行 + stage 时间线 + 内核 console 末行）。panic 会以大字画屏。探针（board.toml `entry_probe_*`）可做二分复位定位。

## 4. 关键设计与已解决问题（时间线）

1. **U-Boot 即固件**：跳过 Mu-Silicium 移植。UniXNU.efi/DarwinBoot.efi 走 FAT 卷文件加载（`loader = "payload"` → unixnu.toml 的 `[[driver]]`）。
2. **U-Boot env**（`~/Xnu4Sirius/unixnu-sirius.env`，`sirius-build.sh ENV_FILE=` 重编）：加 `unixnu_check` 标记检测 + bootmenu 第 2 项。`fastboot flash boot` 刷 `~/Xnu4Sirius/boot-unixnu.img`。
3. **mtools 挂死**：mtools 会开 `/dev/tty` 交互确认（已存在文件时"是否覆盖"）；无终端环境被 SIGTTIN 挂起。`make-uboot-payload.py` 用 `setsid + stdin=DEVNULL + -o` 规避。
4. **链接符号闭包**：DEVELOPMENT 内核无条件引用 `native_legacy_display_probe`（kit 提供 dormant stub）；boot-progress kit 的 extern 全部走弱定义兜底（`check-kit-symbols.py` 可复查）。
5. **BCM2837 panic 地址**：无 `XNU_DT_PANIC_REGION=y` 时内核 bzero 树莓派固定地址 0x3c0fc000 → PE_create_console 内死。defconfig 已加。
6. **stage-3/4 探针二分**：确认 MMU ✓ → serial_init ✓ → PE_create_console ✓（修复后）→ 定位到 IOKit。
7. **GIC-500 预配置（本移植原创）**：XNU Apple 版期待 iBoot 配好的中断控制器；U-Boot 不配置 → 内核停在 lockdown、IRQ=0。`drivers/DarwinBoot`（设备覆盖版）在 ExitBootServices 前按 `/arm-io/intc` 解析 DT 并镜像 ArmCpu 的 initDistributor/wakeRedistributor/enableGic 序列（含 MPIDR 匹配 redistributor、PPI 27 = 虚拟定时器）。验证：内核越过 lockdown 进入 IOKit。
8. **AppleARMCPU 抢占**：无 `sheng-smp=1` 时 ArmCpu::probe 拒绝次级核 nub → kernelcache 里的 AppleARMCPU（IONameMatch=cpu, score 100）接管 → 因缺少 Apple 专有 cpu 属性 panic "CPU configuration in device tree is invalid"。修复：boot args 加 `sheng-smp=1 cpus=8 armcpu-self-ipi=1`（fajita 同款）。
9. **真实 iPad7,1 DT 提取**（`~/Xnu4Sirius/adt-dump.py` + `/tmp/j120.adt`）：IM4P→LZFSE（`~/.local/xnu-linux/bin/lzfse`）→ ADT 解析。注意属性 length 高位是 syscfg 引用标志（`& 0x7fffffff`）。真实 /cpus 属性清单可用于后续补属性。
10. **设备仓 `patches/xnu/` 会整体遮蔽主仓补丁目录**：feeds.py 用 setdefault，同名 patch 模块先到先得（设备仓先注册）。设备仓一旦有 `patches/xnu/`，主仓 57 个常规内核补丁全部失效（SETUP 阶段即崩，hostcompat.h 找不到）。Linux 专用补丁放 **`patches/xnu-linux/`**（独立模块名，不遮蔽），由设备仓覆盖版 `scripts/build-xnu-4k-baseline.sh` 在常规序列之后应用，且带 `uname = Linux` 守卫（linux-host-tools.patch 会重写 SETUP 主机工具规则，在 macOS CI 上无法编译 hostcompat.h，绝不能进 CI）。
11. **诊断内核教训**：给内核链入的对象（ArmCpu 覆盖）加哪怕几十字节的计数器，都会平移整个内核布局并使启动死在 boot-progress 面板之前（同一 DarwinBoot 下，未覆盖版能到面板）。诊断优先走 payload 层（ADT/boot args/DarwinBoot 打印）或 BootProgress 面板行（弱定义符号，不改驱动对象）。
12. **workflow paths 过滤器**已含 drivers/**（踩了三次坑：drivers/ 或 package/ 的改动 push 后 CI 无反应）；concurrency cancel-in-progress 在构建期间 push 会取消正在跑的构建。

## 5. 下一步路线（按优先级）

1. **验证 SMP 修复**（已部署）：看 SMP 行 run=7、stage 推进、停在 root device。
2. **root device 阶段**：预期 panic/wait。可选：加 `XNU_SHENG_PMGR`（AMFI 需要 PMGR，见 fajita `build-sheng-platform-power.py`，注意它需要 `build/native-prelink/` 的参考候选文件，全新树会失败 —— 需要先跑出第一个 prelink 候选或放宽审计）。
3. **显示**：`XNU_NATIVE_LEGACY_DISPLAY`（CPU 渲染显示，fajita 的 `build-native-legacy-display.py` + `drivers/NativeMobileDisplay` 覆盖）。屏参：1080x2244（GOP 2240）、stride 4320、fb 0x9c000000。
4. **触摸**：sirius 触摸 IC 是 SYNAPTICS S3706（主线 DTS `synaptics,s3706`，GENI I2C @0xa94000?）—— 参考 fajita 的 S3706B 驱动（NativeRmi4Touch + NativeGeniI2c，fajita 仓库有完整实现与测试）。
5. **存储（eMMC！）**：本机是 eMMC（sdhci-msm，主线 DTS `sdhc_1: mmc@7c4000`? —— sdm670 是 `mmc@7c4000`? 查 `sdm670.dtsi`）。NativeUfs 不适用。需要新驱动（NativeSdhci，参考 drivers/NativeUfs 的结构 + 主线 `drivers/mmc/host/sdhci-msm.c`），或先用 NativeRamBlock 做 RAM Data 卷跑通用户态（fajita 路线）。
6. **用户态**：trust cache、data seed、`unique_chip_id`（board.toml [payload.adt] 需成对加 chip_id/unique_chip_id）、IPSW OS 卷、launchd…
7. **本地内核编译（工具链已全通，首跑进行中）**：`~/Xnu4Sirius/xnu-linux-toolchain`（Allenwdk）。bootstrap 全部完成，途中三个坑：unifdef 需 `CC=clang`（gcc-15 的 C23 constexpr 关键字撞了 unifdef 的变量名）；tcsh 链接需 `make LIBES='-lcrypt -lncursesw -ltinfo'`（先 `apt install libcrypt-dev`）；cctools-port 一次过（其 ld64 需要 libdispatch —— 从 swift-corelibs-libdispatch swift-5.10-RELEASE 源码用 clang 构建，Ubuntu 无包）。用法：`source env.sh && ./unixnu build kernel`（Linux 补丁由设备仓 baseline 覆盖版自动应用）。两个运行环境要求：网络可达 GitHub API（prepare-xnu-source.py 拉 DriverKit 头，`export https_proxy=http://172.23.0.1:7900`）；**不要在 run_in_background 的管道 stdin 下跑**（migcom 偶发死锁读继承的永不写入的 stdin —— 前台跑或重试即可；bin/ 里 migcom/unifdef 已换成带 /tmp/shim-calls.log 记录的包装脚本）。预期剩余缺口：firehose 私有头（其 README 已知缺口 #3）。通了之后内核迭代不再依赖 CI。

## 6. 环境清单

- **WSL**（Ubuntu 26.04, 32 核）：git/clang21/lld/mtools/dtc/unifdef/cmake/ninja/g++/flex/bison、`gh`（账号 57UU，已登录）。
- **Windows 侧**：`fastboot.exe`（Git Bash 直接用；wsl 路径用 `//wsl.localhost/Ubuntu-26.04/...`）。
- **手机**：boot 分区 = unixnu U-Boot；vendor(p80) = payload FAT；system(p79) = Linux 内核（勿动）；userdata(p81) = Linux rootfs。SSH u57u@172.16.42.1（密钥已铺，sudo 密码 1234）。
- **CI**：GitHub Actions macos-26 runner（公开仓库免费），产物 `sirius-kernel-<N>`，retention 3 天，及时下载。

## 7. 常见坑速查

| 症状 | 原因/解法 |
| --- | --- |
| mtools 命令永久挂起 | `/dev/tty` 交互 + SIGTTIN；`setsid` + `< /dev/null` + `-o` |
| fastboot 显示 `????????????` | USB 会话坏；拔插线或重启手机进 fastboot |
| CI 没触发 | push 的路径不在 workflow paths 过滤器；手动 `gh workflow run` |
| CI 构建被取消 | concurrency cancel-in-progress；构建期间别 push |
| payload 补丁报 "padding ends at 0x..." | stage-3 洞 + panic screen 超出内核 padding；二分时关 panic_screen |
| 内核停在 lockdown、IRQ=0 | GIC 未预配置（DarwinBoot PrepareGic，本仓库已含） |
| "CPU configuration in device tree is invalid" | 缺 `sheng-smp=1`（次级核被 AppleARMCPU 抢走） |
| 屏幕只有色带没日志 | boot-progress 未编入或 `boot-progress=2` 参数缺失 |

## 8. 仓库结构（本仓库）

```
board.toml                        # 板级配置（内核参数、payload、ADT 参数、探针）
configs/sirius_defconfig          # 内核配置（第一控制台 + boot progress + DT panic region）
drivers/DarwinBoot/               # 覆盖版 XNU 加载器（+GIC 预配置）
package/                          # kit：boot-progress 组件 + dormant stubs + Font.h
scripts/
  make-sirius-adt.py              # Apple DT 生成器（SDM710 值）
  make-uboot-payload.py           # U-Boot 路线 payload 终装
  build-boot-progress.py          # boot-progress 归档构建
  build-sirius-probes.py          # dormant probe stub 归档
  build-xnu-4k-baseline.sh        # 覆盖版（放宽 boot-progress 的 UFS interlock）
.github/workflows/kernel.yml      # macOS 内核 CI
docs/                             # linux-kernel-build.md、本文档
```
