# UniXNU on Xiaomi Mi 8 SE (sirius) — 交接文档

更新：2026-10-25。本文档交接"把 UniXNU（XNU 内核 + iPadOS 用户态）移植到小米 8 SE"的全部上下文：当前进度、架构决策、构建/部署循环、已解决的问题、下一步路线。

---

> 本文只保留**当前状态、构建流程、环境与坑**。逐阶段的历史记录（2026-10-25 → 10-27，
> 从首个 `bsd_init` 到 eMMC、根挂载、init、execve）在 `docs/archive/HANDOFF-stages.zh-CN.md`。

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

## 2. 当前状态（2026-10-27）

| 阶段 | 状态 | 证据 |
| --- | --- | --- |
| UEFI（U-Boot） | ✅ | `boot-unixnu.img`：原版 U-Boot + 自定义默认环境（`unixnu_check`/`unixnu_boot`），标记文件在 p80 时自动进 UniXNU |
| payload 卷 | ✅ | p80 FAT，`\EFI\UniXNU\BOOTUNIXNU` 标记 |
| 内核入口 / MMU / 定时器 / PSCI | ✅ | 入口探针色带、`smp=1 num=7` |
| 控制台 | ✅ | boot-progress 面板 + CDC-ACM USB 环；内核 `printf` 与 panic 文本都进环 |
| IOKit | ✅ | `sheng-smp=1` 让原生 ArmCpu 抢下 `/cpus`，并发布 `ApplePMGRNub`；`disk0/disk1s1/disk2s1` 与 AMFI 均起来 |
| 存储（eMMC） | ✅ | SDCC1；SHNGROOT 窗口（OS/SystemOS）发布为块设备；整盘 0x747BFDF 扇区；GPT 由 IOStorageFamily 解析 |
| 根文件系统 | ✅ | APFS 挂载 OS 卷 `disk2s1`；`bsd_init` 完整跑完；sealed-root 答案记录后继续 |
| init / launchd | ✅ | `execve` 全链路通过（`cloneproc`→`proc locks`→`activate`→`namei 0`），`execve activate 0` |
| 信任链 | ✅ | 原始 bundle（`7416ca4f…`，两个未改动模块）打进 payload，启动期初始化已补回；上机确认 AMFI 直接放行（`execve activate 0`，且 `exec policy answered` 不再出现），`sirius-skip-cs` 已从 boot args 移除 |
| 用户态 | ⏳ | exec 成功后驱动不再收到请求，剩余等待在存储栈之外 |
| 时钟 | 🔶 | 400 kHz → 25 MHz 已上机验证（`EMMC clock25=ok`）；50 MHz 需 CMD6 高速模式 |

**最新一次部署**：kernel `75a4a4da…`（含 25 MHz、启动期信任链初始化、原始 bundle），payload 内 `Payloads/kernel.macho.trustcache`。

**恢复 Linux**：`fastboot flash boot ~/Projects/sirius-distro/build/boot.img`（原版 U-Boot，不认标记），之后再刷回 `boot-unixnu.img`。

## 3. 构建-部署-测试循环

### 3.1 U-Boot（`boot-unixnu.img`，只在改 U-Boot 或它的启动逻辑时需要）

环境文件与本说明已版本化在 [`uboot/`](uboot/README.md)（`uboot/unixnu-sirius.env`），
可以只凭本仓库 + `sirius-distro` 复现这个镜像。

U-Boot 本身**没有改源码**：用的是 `sirius-distro` 的原版 U-Boot（qcom_defconfig +
qcom-phone + sirius 配置，带 EFI_LOADER/GOP）。UniXNU 的引导逻辑完全写在一个**默认环境文本文件**里，
由 `sirius-build.sh` 通过 `CONFIG_ENV_DEFAULT_ENV_TEXT_FILE` 编进镜像：

```bash
cd ~/Projects/sirius-distro
ENV_FILE=$PWD/../sirius/uboot/unixnu-sirius.env ./uboot/sirius-build.sh
# 产物 boot.img（U-Boot 镜像），另存为 boot-unixnu.img：
#   cp boot.img ~/Projects/Xnu4Sirius-legacy/boot-unixnu.img
fastboot flash boot ~/Projects/Xnu4Sirius-legacy/boot-unixnu.img
```

环境文件里与 UniXNU 相关的只有四行（其余是 Linux 引导用的）：

```
unixnu_boot   = load mmc 0:50 ${kernel_addr_r} /EFI/BOOT/BOOTAA64.EFI; bootefi ${kernel_addr_r}:${filesize}
unixnu_check  = if load mmc 0:50 ${scriptaddr} /EFI/UniXNU/BOOTUNIXNU; then run unixnu_boot; fi
bootmenu_2    = Boot UniXNU (p80 vendor)=run unixnu_boot
bootcmd       = run check_bcb; run unixnu_check; run do_boot
```

要点：

- `mmc 0:50` 是**十六进制**分区号（`0x50`=80=p80 vendor）；写 `0:80` 会被当成 `0x80`=128 → `Invalid partition 128`。
- 标记文件 `/EFI/UniXNU/BOOTUNIXNU` 决定**这一次启动**走 UniXNU；没有标记就照旧进 Linux。
  这就是 `make-uboot-payload.py --marker` 写进 payload 的那个空文件。
- 不要用 `bootflow scan`：每盘只扫前 30 个分区，p80 扫不到；`sysboot`/`load` 直指分区最稳。

### 3.2 内核（改 defconfig/kit/补丁/驱动时需要 —— macOS CI）

```bash
cd ~/Projects/Xnu4Sirius/sirius
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

### 3.3 EFI 程序（本地 WSL，秒级）

```bash
cd ~/Xnu4Sirius/work/UniXNU
./unixnu build package        # UniXNU.efi（已构建过，一般不用重跑）

# DarwinBoot.efi（本仓库覆盖版，含 GIC 预配置）：
cd ~/Projects/Xnu4Sirius/sirius/drivers/DarwinBoot
BASE=$HOME/Xnu4Sirius/work/UniXNU/out/feeds/mu_basecore-89034cbb
WORKSPACE=$PWD PACKAGES_PATH=$PWD:$HOME/Xnu4Sirius/work/UniXNU/package:$BASE \
EDK_TOOLS_PATH=$BASE/BaseTools \
CONF_PATH=$HOME/Xnu4Sirius/work/UniXNU/out/sirius/package/Build/Conf \
CLANG_BIN=/usr/bin/ PYTHON_COMMAND=python3 \
PATH=$BASE/BaseTools/BinWrappers/PosixLike:$BASE/BaseTools/Source/C/bin:$PATH \
build -p DarwinBootPkg/DarwinBoot.dsc -a AARCH64 -b DEBUG -t CLANGPDB -n 32
# 产物: build/DarwinBoot/DEBUG_CLANGPDB/AARCH64/DarwinBoot.efi（~7 秒）
```

### 3.4 payload 组装（本地，约 1 分钟）

```bash
cd ~/Xnu4Sirius/work/UniXNU
UNIXNU_UEFI=$HOME/Xnu4Sirius/work/uefi-sheng \
./unixnu build payload --kernel ~/Xnu4Sirius/kernel-artifact/kernel-<name>.macho \
  --trust-cache build/original-static-system-trust/original-static-system.bundle
python3 ~/Xnu4Sirius/UniXNU-sirius/scripts/make-uboot-payload.py \
  --payload out/sirius/payload/payload.img \
  --unixnu-efi out/sirius/package/Build/Out/AARCH64-DEBUG/UniXNU.efi \
  --darwinboot-efi ~/Xnu4Sirius/UniXNU-sirius/drivers/DarwinBoot/build/DarwinBoot/DEBUG_CLANGPDB/AARCH64/DarwinBoot.efi \
  --marker --output ~/Xnu4Sirius/payload-full.img
```

- `--marker` 写入 BOOTUNIXNU（自动启动 UniXNU；去掉则开机默认 Linux）。
- ⚠️ `--trust-cache` **不能忘**：漏掉时 payload 里没有 `kernel.macho.trustcache`，内核日志会打
  `no external trust caches found (segment length is zero)`，AMFI 于是用 namespace 9 / code 8
  杀掉 launchd（panic 屏显示 `unexpected SIGKILL of launchd`）。忘了就重刷，不是内核回归。
- `UNIXNU_UEFI` 提供 panic-screen 探针需要的 Font.h（uefi-sheng 检出）。
- `unixnu build payload` 会重装 `out/sirius/tree`（脚本从设备仓覆盖主仓）；payload patch 会打印 `panic screen: ...`，并会做指令级校验，偏移不符会直接拒绝。

### 3.5 真机部署

```bash
# 手机侧进入 fastboot：UniXNU 菜单选 Power off → 音量下+电源；Linux 里 sudo reboot bootloader
# Windows 侧（Git Bash）：
fastboot.exe flash vendor "//wsl.localhost/Ubuntu-26.04/home/u57u/Xnu4Sirius/payload-full.img"
fastboot.exe reboot
```

恢复 Linux：fastboot 下 `fastboot flash boot ~/sirius-distro/build/boot.img`（原版 U-Boot，不认标记，直接进 Linux），操作完再把 `~/Projects/Xnu4Sirius-legacy/boot-unixnu.img` 刷回去。手机回到 Linux 后 SSH：`ssh u57u@172.16.42.1`（免密已铺）。

### 3.6 观察输出

屏幕即调试器：boot-progress 面板（CPU/SMP/CLK/MEM/GIC/TOPO/PROBE 各行 + stage 时间线 + 内核 console 末行）。panic 会以大字画屏。探针（board.toml `entry_probe_*`）可做二分复位定位。

面板布局注意（实测）：

- GOP 宽度 1080 → `display.scale=1`，每行 14 px，48 行占 672 px。
- panic screen 从 `height/8`（2244/8 = 280 px ≈ 第 20 行）往下清屏；**第 0..19 行在 panic 后仍可见**。
- 屏顶部有圆角/刘海，**不要把关键数据放 0..3 行**；推荐第 8/9 行。
- monitor 线程只画一帧就 `assert_wait_timeout`（依赖 ArmCpu 的定时器）；没有定时器时面板会冻住。panic 时刻的可信快照靠 §4.14 的强制刷新。

## 4. 关键设计（现状）

1. **U-Boot 即固件**：不移植 Mu-Silicium。U-Boot 的默认环境换成 `unixnu-sirius.env`
   （`unixnu_check` 看 p80 上的 `BOOTUNIXNU`，`unixnu_boot` 加载 `/EFI/BOOT/BOOTAA64.EFI`），
   `bootcmd=run check_bcb; run unixnu_check; run do_boot` → 有标记进 UniXNU，否则回 Linux。
2. **GIC 预配置**：XNU 期待 iBoot 配好中断控制器；`drivers/DarwinBoot` 在 ExitBootServices 前
   按 `/arm-io/intc` 配置 distributor/redistributor，并**只使能 SGIs**（timer PPI 交给 ArmCpu 自己开）。
3. **定时器 INTID**：SDM670/710 的虚拟定时器是 PPI 3 = INTID 19（不是 Apple 的 27）。
4. **CPU 抢占**：`sheng-smp=1` 让原生 ArmCpu 抢下所有 `/cpus` nub，否则 kernelcache 的
   AppleARMCPU 接管并在 `_validateConfiguration()` panic。
5. **AMFI 的 PMGR**：`sheng-pmgr=2` 发布 `ApplePMGRNub`，AMFI 的启动依赖它。
6. **控制台**：boot-progress 面板 + `sirius-usb=2` 的 CDC-ACM 环；`sirius-console-ring.patch`
   把内核 `printf`（含 panic 文本）镜像进环，这是后来所有定位的基础。
7. **存储**：SDCC1 + SHNGROOT 清单把 `[userdata]` 里的 OS/SystemOS 窗口发布成块设备；
   IOStorageFamily 是 kernelcache 的 kext，导入地址由 `derive-storage-bindings.py` 从原始
   kernelcache 推导并按 prelink slide 绑定。
8. **补丁目录**：设备仓 `patches/xnu/` 会**整体遮蔽**主仓补丁目录，Linux 专用补丁放
   `patches/xnu-linux/`。

## 5. 下一步路线（按优先级）

1. **信任链回归**：换 IPSW 时要重新生成 bundle 并更新 `board.toml [payload] trust_cache_sha256`；
   `sirius-skip-exec-policy.patch` 保留但不再启用，作为退路。
2. **exec 之后的停顿（fault 循环，指向 pmap/地址空间不切换）**：探针显示两种表现，都发生在
   "刚映射完的页 CPU 仍然访问不到"：i) `copyout()` 里 pid 1 线程连续取 fault（一分钟 6267 万次），
   第一次 level-1 translation、之后全是 level-3 permission，而 `pmap_find_phys` 找得到页、
   `AT S1E1W` 能翻译、`vm_fault` 返回 `KERN_SUCCESS`；ii) exec 成功后 launchd 线程的入口
   `0x100924e50` 已经拿到（`SIRIUS user thread start pc`），但它在用户态做**取指**
   时对 3 个地址反复拿 instruction abort（ESR 0x82000007，level-3 translation fault），
   一秒钟几百万次，且**一条 syscall 都没发出、读请求冻结在 0x10D**。
   两者的共同解释是：`vm_fault` 把页映射进 `thread->map` 的 pmap，而 CPU 的 TTBR0 仍指向
   另一个 pmap（同一地址在 `AT S1E1W` 与 `pmap_find_phys` 上给出不同物理页，已经量到）。
   下一步：在 exec 的 `proc_exec_switch_task` 之后确认/强制 pmap 切换（或对比
   `TTBR0_EL1` 与 `thread->map->pmap` 的根表），这是当前最可能的根因。
   附带：这几轮 payload 曾漏掉 `--trust-cache`，AMFI 因此以 namespace 9/code 8 杀掉 launchd
   （`no external trust caches found`）；带信任链重刷后 exec 正常走到
   `init execve returned 0` + `execve activate 0`，AMFI 不再报 `exec policy`。
3. **自动进 fastboot（只解决了一半）**：BCB 写 `uboot-fastboot` 能让下次启动进 U-Boot 自己的
   fastboot（已上机验证两次），但**从它那里刷分区会挂住**：`f_fastboot.c` 每次 queue 前先 dequeue
   （本机构建里已经删掉这一行并提交到 u-boot 子模块本地），刷 vendor 仍然挂，说明根因还在
   dwc3 的 ENDTRANSFER/STARTTRANSFER 与缺 `DWC3_DEPCMD_STATUS` 处理（子代理对比 Linux 的 dwc3 给出的
   结论）。ABL 真正读的是 PMIC spare（`PON_SOFT_RB_SPARE` 0x88f=0x04），写它需要 `CONFIG_CMD_PMIC`
   加上确认 `pmic` 设备名；这条还没上机验证，所以现在仍是手动 Vol-Down + Power 进 ABL fastboot。
   另外：U-Boot fastboot 会回报 `current-slot a`，主机因此把分区名加上 `_a`，环境里已用
   `fastboot_partition_alias_<name>_a=<name>` 兜住。
4. **时钟到 50 MHz**：25 MHz 已验证；50 MHz 需要在 `emmc_set_card_clock_25m` 之后加 CMD6
   （SWITCH，`HS_TIMING=1`），失败要能退回 25 MHz。
5. **用户态后续**：data seed（`--data-ramdisk`）、`unique_chip_id`、dyld 缓存读取量、显示与触摸。

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

## 9. 提交历史

提交历史以 `git log` 为准；逐阶段记录在 `docs/archive/HANDOFF-stages.zh-CN.md`，
早期设计笔记在 `docs/archive/HANDOFF-design-notes.zh-CN.md`。
