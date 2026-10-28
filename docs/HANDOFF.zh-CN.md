# UniXNU on Xiaomi Mi 8 SE (sirius) — 交接文档

更新：2026-10-28。本文档交接"把 UniXNU（XNU 内核 + iPadOS 用户态）移植到小米 8 SE"的当前状态：
架构决策、构建/部署/观测回路、关键设计、下一步和坑。**只写现在成立的东西**；逐阶段历史
（2026-10-25 → 10-28，从首个 `bsd_init` 到 eMMC、APFS、execve、PAN）在
`docs/archive/HANDOFF-stages.zh-CN.md`，早期设计笔记在 `docs/archive/HANDOFF-design-notes.zh-CN.md`。

## 1. 项目概况

| 项 | 内容 |
| --- | --- |
| 目标设备 | 小米 8 SE（sirius，Snapdragon 710 / Kryo 360，6 GB eMMC 机型） |
| 目标系统 | [UniXNU](https://github.com/UniXNU/UniXNU)：XNU 23.6.0（4 KiB BCM2837 personality）+ 预链接 iPad7,1 iPadOS 17.7.11 (21H461) 内核缓存 |
| 设备仓库 | https://github.com/57UU/UniXNU-sirius（本仓库：`board.toml` 板级配置，`porting.toml` 机器校验的移植记录） |
| 工作区 | `~/Projects/Xnu4Sirius/`：`sirius/`（本仓库）、`UniXNU/`（框架检出，`out/`、`feeds/` 都在这）、`iPad_Pro_HFR_17.7.11_21H461_Restore.ipsw` |
| 旧工作区 | `~/Projects/Xnu4Sirius-legacy/`：`legacy` 分支的检出与旧产物，只作参考 |
| 参考端口 | UniXNU-fajita（OnePlus 6T，SDM845）、UniXNU-sheng（同代、同为 iPadOS 17 用户态） |

**核心架构决策：用设备自带的 U-Boot（sirius-distro，U-Boot 2026.07，EFI_LOADER 全开）替代 Mu-Silicium 固件**。引导链：

```
ABL (boot 分区 = 重编的 U-Boot boot.img)
  └→ U-Boot UEFI (GOP/vidconsole + FAT 块设备 + EFI_LOADER)
      └→ 标记检测: p80 (vendor 分区 FAT) 有 /EFI/UniXNU/BOOTUNIXNU 就启动 UniXNU，否则正常启动 Linux
          └→ \EFI\BOOT\BOOTAA64.EFI = UniXNU.efi（菜单）
              ├→ unixnu.toml 配置（payload 卷根）
              ├→ [[driver]] DarwinBoot.efi（XNU 加载器，本仓库 drivers/DarwinBoot 覆盖版）
              │    ├→ GIC-500 预配置（iBoot 角色，见 §4）
              │    └→ 加载 kernel.macho（预链接 XNU）+ kernel.macho.adt → ExitBootServices → DarwinEnter
              └→ XNU → boot-progress 面板（大字体实时内核日志）
```

## 2. 当前状态（2026-10-28）

| 阶段 | 状态 | 证据 |
| --- | --- | --- |
| UEFI（U-Boot） | ✅ | `uboot/boot-unixnu.img`：原版 U-Boot + 自定义默认环境（`unixnu_check`/`unixnu_boot`/`unixnu_arm`），标记文件在 p80 时自动进 UniXNU |
| payload 卷 | ✅ | p80 FAT（label `SIRIUSESP`，256 MiB）：`\EFI\UniXNU\BOOTUNIXNU` 标记 + `BOOTAA64.EFI` + `Payloads/{kernel.macho,kernel.macho.adt,kernel.macho.trustcache,DarwinBoot.efi}` |
| 内核入口 / MMU / 定时器 / PSCI | ✅ | 入口探针色带、`smp=1 num=7` |
| 控制台 | ✅ | boot-progress 面板 + CDC-ACM USB 环；内核 `printf` 与 panic 文本都进环 |
| IOKit | ✅ | `sheng-smp=1` 让原生 ArmCpu 抢下 `/cpus`，并发布 `ApplePMGRNub`；`disk0/disk1s1/disk2s1` 与 AMFI 均起来 |
| 存储（eMMC） | ✅ | SDCC1；SHNGROOT 窗口（OS/SystemOS）发布为块设备；整盘 0x747BFDF 扇区；GPT 由 IOStorageFamily 解析 |
| 根文件系统 | ✅ | APFS 挂载 OS 卷 `disk2s1`；`bsd_init` 完整跑完；sealed-root 答案记录后继续 |
| init / launchd | ✅ | `execve` 全链路通过（`cloneproc`→`proc locks`→`activate`→`namei 0`），`execve activate 0` |
| 信任链 | ✅ | 原始 bundle（`7416ca4f…`，StaticTrustCache + Cryptex1,SystemTrustCache）打进 payload；AMFI 直接放行，`sirius-skip-cs` 已从 boot args 移除 |
| 用户态 | 🔶 | exec 后跑起来的是 **dyld 的 ignition**，不是 launchd 本体；**24 条是 trace 上限**，实际进入 31 条、返回 30 条。第 26 条 syscall 是 `sysctl` `security.mac.img4.ignition_blob`，pid 1 线程停在 `sysctl_root`/`sysctl`，用户 PC 为 dyld `__sysctl` 的 `svc` 后一指令 |
| Image4 ignition | 🔶 | 停点已定位：AppleImage4 的 handler 以**无超时** `waitForMatchingService` 等 `IOAESAccelerator`，SDM710 没有 AES 硬件。已启用 SoftwareAES（`CONFIG_XNU_NATIVE_AES=y` + `board.toml` 的 `build-arm-driver.py SoftwareAES` / `build-software-aes-bindings.py`），待上机验证 |
| dyld 共享缓存 | ✅ | SystemOS cryptex 卷（UUID `8846FE78-2910-4297-AF97-D59BB6F8FC16`）以 `/dev/disk4s1` 只读 union 叠到 `/`（flags `0x14809021`，挂载表 2→3）；缓存目录与 `dyld_shared_cache_arm64` 的 probe 从 ENOENT 变 0（2026-10-28 14:50 上机验证） |
| 时钟 | 🔶 | 400 kHz → 25 MHz 已上机验证（`EMMC clock25=ok`）；50 MHz 需 CMD6 高速模式 |

**最近一次部署**：kernel `0ff05f7f…`（union + launchd 线程采样 + syscall/sysctl trace），
`payload-full.img` sha256 `8cd27069…`，boot args 含 `ignition_level=1`、`launchd_unsecure_cache=1`、
`sirius_systemos_union=1`。14:50 的日志证实 union 生效（`docs/evidence/sirius-systemos-union-20261028-1450.log`）；
15:22 的日志把停点定位到 Image4 ignition（`docs/evidence/sirius-image4-wait-20261028-1522.log`）：
实际 31 条 syscall，最后一条 `security.mac.img4.ignition_blob` 不返回。NativeAES 构建已在进行；
刷机/上机结果会替换这一行。

**恢复 Linux**：fastboot 下 `fastboot flash boot ~/Projects/sirius-distro/build/boot.img`（原版 U-Boot，
不认标记），操作完再把 `uboot/boot-unixnu.img` 刷回去。

## 3. 构建-部署-测试回路

全部在本机（Fedora 主机）完成，**不需要 CI**；`.github/workflows/kernel.yml` 只是可选的 macOS 备份路径。

### 3.1 内核（改 defconfig/kit/补丁/驱动时）

内核在 podman 容器里用框架自带的 Linux 工具链构建（原理见 `docs/linux-kernel-build.md`）：

```bash
cd ~/Projects/Xnu4Sirius/sirius
export XDG_RUNTIME_DIR=/tmp/xdgrt          # 否则 podman 报 /run/user/1000/libpod 只读
./scripts/xnu-container.sh run bash -lc '
  cd ../UniXNU && ./unixnu defconfig sirius_defconfig -D ../sirius && ./unixnu build kernel -D ../sirius'
# 产物: UniXNU/out/sirius/ipados-17.7.11-21H461/sirius/kernel/kernel.macho（+ kernel.json）
# 日志: 同目录 logs/kernel.log，末尾打印 kernel.macho 的 sha256
```

- 镜像 `localhost/unixnu-xnu-linux:fedora44`，一次性 `./scripts/xnu-container.sh build-image`。
- 增量数分钟，全量约 10 分钟；工作区按绝对路径挂载，`out/` 里的编译记录可以续用。
- 落地位置：补丁放 `patches/xnu/` 并在 `patches/xnu/series` 登记；kit 源码放 `package/`（构建时以
  `kits/sirius` 出现在树里）；`board.toml` 的 `[kernel] archives` 决定编哪些。

### 3.2 payload 卷（改内核/ADT/启动参数后都要重做）

```bash
cd ~/Projects/Xnu4Sirius/UniXNU
./unixnu build payload --kernel out/sirius/ipados-17.7.11-21H461/sirius/kernel/kernel.macho \
  --trust-cache build/original-static-system-trust/original-static-system.bundle
python3 ../sirius/scripts/make-uboot-payload.py \
  --payload out/sirius/ipados-17.7.11-21H461/sirius/payload/payload.img \
  --unixnu-efi out/sirius/ipados-17.7.11-21H461/sirius/package/Build/Out/AARCH64-DEBUG/UniXNU.efi \
  --darwinboot-efi out/sirius/ipados-17.7.11-21H461/sirius/tree/build/DarwinBoot/DEBUG_CLANGPDB/AARCH64/DarwinBoot.efi \
  --marker --output ~/Projects/Xnu4Sirius/payload-full.img
```

- `--marker` 写 `BOOTUNIXNU`（自动启动 UniXNU；去掉则开机默认 Linux）。
- ⚠️ `--trust-cache` **不能忘**：漏掉时 payload 里没有 `kernel.macho.trustcache`，内核会打
  `no external trust caches found (segment length is zero)`，AMFI 以 namespace 9 / code 8 杀掉 launchd
  （panic 屏 `unexpected SIGKILL of launchd`）。忘了就重做 payload 重刷，不是内核回归。
- 最后一步脚本还会把 AppleARMCPU 的 personality 改名（否则它抢 `/cpus` 并 panic），并做指令级校验。

### 3.3 EFI 程序（只在改 UniXNU.efi / DarwinBoot 时需要）

- `UniXNU.efi`：`cd ~/Projects/Xnu4Sirius/UniXNU && ./unixnu build package`（EDK II 包已 vendor 在
  `sirius/efi/`，也可 `cd sirius/efi && ./build.sh`）。
- `DarwinBoot.efi`（本仓库覆盖版，含 GIC 预配置）：`bash <tree>/scripts/build-darwinboot.sh`，产物在
  `<tree>/build/DarwinBoot/DEBUG_CLANGPDB/AARCH64/DarwinBoot.efi`（秒级，一般不用重跑）。

### 3.4 U-Boot（只在改 U-Boot 或它的启动逻辑时需要）

U-Boot 源码**没有改**：用 `uboot/src`（submodule，`57UU/u-boot` 的 `sirius/v2.0`）里的原版，UniXNU 的
引导逻辑全在默认环境文本 `uboot/unixnu-sirius.env` 里：

```
unixnu_arm   = if bcb load 0 misc; then bcb set command uboot-fastboot; bcb store; fi
unixnu_boot  = run unixnu_arm; load mmc 0:50 ${kernel_addr_r} /EFI/BOOT/BOOTAA64.EFI; bootefi ${kernel_addr_r}:${filesize}
unixnu_check = if load mmc 0:50 ${scriptaddr} /EFI/UniXNU/BOOTUNIXNU; then run unixnu_boot; fi
bootmenu_2   = Boot UniXNU (p80 vendor)=run unixnu_boot
bootcmd      = run check_bcb; run unixnu_check; run do_boot
```

```bash
cd ~/Projects/Xnu4Sirius/sirius && ./uboot/build.sh      # → uboot/boot-unixnu.img
fastboot flash boot uboot/boot-unixnu.img
```

要点：`mmc 0:50` 是**十六进制**分区号（`0x50`=80=p80）；写 `0:80` 会被当成 `0x80`=128。不要用
`bootflow scan`（每盘只扫前 30 个分区，p80 扫不到）。

### 3.5 真机部署与观测

```bash
# 手机进 ABL fastboot：UniXNU 菜单 Power off → 音量下 + 电源
cd ~/Projects/Xnu4Sirius/sirius
python3 scripts/flash-when-fastboot.py ~/Projects/Xnu4Sirius/payload-full.img --timeout 1800 --poll 5
```

脚本自己轮询 `fastboot devices`，只对 `product: sirius` 且 `version-bootloader` 为空的 ABL fastboot 动手
（`fastboot` 的报告走 **stderr**，脚本按此解析）；落进 U-Boot 自己的 fastboot 时它会 `fastboot erase misc`
+ `reboot` 绕回 UniXNU。

观测（屏幕即调试器，USB 环给全文）：

```bash
sudo -n python3 scripts/capture-usb-console.py --port /dev/ttyACM0 --duration 20 \
  --output ~/Projects/Xnu4Sirius/.scratch/console.log
```

- `/dev/ttyACM0` 属 root:dialout，用户不在 dialout 组，**必须 sudo**（本机 sudo 免密）。
- 驱动在收到主机任意字节时会把整个环重放一遍，所以随时抓都能拿到完整启动日志。
- 面板布局：GOP 宽 1080 → `display.scale=1`，每行 14 px；panic 屏从 `height/8`（≈第 20 行）清屏，
  第 0..19 行仍是面板；顶部有刘海，关键数据放第 8/9 行。

## 4. 关键设计（现状）

1. **U-Boot 即固件**：不移植 Mu-Silicium；`bootcmd=run check_bcb; run unixnu_check; run do_boot`。
2. **GIC 预配置**：XNU 期待 iBoot 配好中断控制器；`drivers/DarwinBoot` 在 ExitBootServices 前按
   `/arm-io/intc` 配置 distributor/redistributor，并**只使能 SGIs**（timer PPI 交给 ArmCpu 自己开）。
3. **定时器 INTID**：SDM670/710 的虚拟定时器是 PPI 3 = INTID 19（不是 Apple 的 27）。
4. **CPU 抢占**：`sheng-smp=1` 让原生 ArmCpu 抢下所有 `/cpus` nub；`sheng-pmgr=2` 发布 `ApplePMGRNub`
   （AMFI 启动依赖它）。
5. **PAN**：SDM710 报 FEAT_PAN2，每次进 EL1 都置 PAN，所以必须 `CONFIG_XNU_NATIVE_PAN=y`；否则所有
   内核访问用户页都吃 permission fault（当时的证据与弯路见 archive）。
6. **控制台**：boot-progress 面板 + `sirius_usb=2` 的 CDC-ACM 环；`sirius-console-ring.patch` 把内核
   `printf`（含 panic 文本）镜像进环，这是后来所有定位的基础。
7. **存储**：SDCC1 + SHNGROOT 清单把 `[userdata]` 里的 OS/SystemOS 窗口发布成块设备；IOStorageFamily 是
   kernelcache 的 kext，导入地址由 `derive-storage-bindings.py` 从原始 kernelcache 推导并按 prelink slide
   绑定。驱动只读，传输单飞（原子标志 + IODelay；读路径不可睡，不能用 IOLock）。
8. **根卷与 cryptex**：根是 OS 卷（APFS `disk2s1`，UUID `04769D26-…`），dyld 的共享缓存却在 SystemOS
   cryptex 卷（`8846FE78-…`）。本 build 的 `CONFIG_CSR=0`（`config_csr.h` 为 `#define NCONFIG_CSR 0`），
   `vm_unix.c` 走"缓存在进程根挂载上（否则只认 `/private/preboot/Cryptexes`）"的分支——`/chosen/asmb`
   （lp-sip0/lp-sip1）只对 `CONFIG_CSR≠0` 有意义，本板不适用。`sirius-systemos-union.patch` 因此在
   exec init 之前把 SystemOS 卷以 `MNT_RDONLY|MNT_UNION` 叠到 `/`（iPadOS 自己也是这个组合方式），
   缓存路径就落在根挂载上。UniXNU 自带的 `XNU_OWNER_CACHE_MOUNT` 走不通：它要求
   `XNU_OWNER_ROOT_TRUST=1` 和 `native_verified_block_*`（owner-root 信任链），本板没有。
9. **补丁目录**：设备仓 `patches/xnu/` 会**整体遮蔽**主仓补丁目录；Linux 专用补丁放 `patches/xnu-linux/`。
10. **Image4 ignition 与软件 AES**：dyld 的 libignition 通过 `security.mac.img4.ignition_blob` 取 ignition
    blob；AppleImage4 kext 的 handler 会 `waitForMatchingService("IOAESAccelerator")`，超时参数是
    `(1<<64)-1`（无超时）。SDM710 没有 AES 硬件，所以必须编入 UniXNU 的 `NativeAES`（SoftwareAES）软件
    实现，让它在 handler 之前发布 `IOAESAccelerator`；否则这个 sysctl 永不返回，用户态停在这里。

## 5. 下一步路线（按优先级）

1. **验证 SoftwareAES 解开 Image4 等待（当前这一步）**。NativeAES 已编入（见 §2 Image4 行），刷完看：
   - `sirius: sysctl mib 101.100.118.100` 之后是否出现 `sirius: init syscall 202 return 0`；
   - `NEMMC launchd pc` 是否离开 dyld 的 `__sysctl` 返回地址，`NEMMC probe init syscalls` 是否 > 31；
   - 若仍停在 sysctl，确认 `NativeAES` 真的 start（KAT/`native_aes_registered` 探针）；若 sysctl 返回
     错误而 libignition 继续，那说明 ignition blob 在 level 1 是可选的，也接受。
   备选（不首选）：让 AppleImage4 handler 在缺 `IOAESAccelerator` 时返回 `ENOTSUP`；改原 kext 不现实，
   软件 provider 是干净路径。
2. **时钟到 50 MHz**：25 MHz 已验证；50 MHz 需在 `emmc_set_card_clock_25m` 之后加 CMD6
   （SWITCH，`HS_TIMING=1`），失败要能退回 25 MHz。
3. **自动进 fastboot**：BCB 写 `uboot-fastboot` 能让下次启动进 U-Boot fastboot，但**从它那里刷分区会挂住**
   （dwc3 的 ENDTRANSFER/STARTTRANSFER 与缺 `DWC3_DEPCMD_STATUS` 处理）。ABL 真正读的是 PMIC spare
   （`PON_SOFT_RB_SPARE` 0x88f=0x04），需要 `CONFIG_CMD_PMIC` 加确认 `pmic` 设备名，尚未上机验证。
   所以现在仍是手动 Vol-Down + Power 进 ABL fastboot。
4. **用户态往后**：缓存通了之后是 Data seed（`--data-ramdisk`）、`unique_chip_id`、显示与触摸。
5. **信任链回归**：换 IPSW 时要重新生成 bundle 并更新 `board.toml [payload] trust_cache_sha256`；
   `sirius-skip-exec-policy.patch` 保留但不再启用，作为退路。

## 6. 环境清单

- **主机**：Fedora 44（`tianxuan6`），`sudo` 免密；`fastboot`/`adb`、`podman`、`mtools`、`dtc`、
  `clang`/`lld`、`gh`（账号 57UU，已登录）。
- **容器**：`localhost/unixnu-xnu-linux:fedora44`（内核工具链；`XDG_RUNTIME_DIR=/tmp/xdgrt` 必需）。
- **手机**：boot 分区 = unixnu U-Boot；vendor(p80) = payload FAT；system(p79) = Linux 内核（勿动）；
  userdata(p81) = Linux rootfs + SHNGROOT 镜像集。Linux 起来后 SSH `u57u@172.16.42.1`。
- **IPSW**：工作区根 `iPad_Pro_HFR_17.7.11_21H461_Restore.ipsw`；解出的文件在
  `UniXNU/out/ipsw/21H461/`：`kernelcache.decompressed`、`044-76040-038.dmg`（OS）、
  `044-76034-038.dmg`（SystemOS）。
- **网络**：本机构建不需要外网；`gh` 直连即可（`gh auth token` 可用）。

## 7. 常见坑速查

| 症状 | 原因/解法 |
| --- | --- |
| `fastboot devices` 空、`lsusb` 无 `18d1` | 手机没枚举（关机/线松）；插回原 USB 口再看 |
| 刷完 vendor 重启进了 fastboot，`getvar product` = `mtp`、`version-bootloader` = `U-Boot …` | 这是 U-Boot 自己的 fastboot，**不能写分区**；`fastboot erase misc` + `fastboot reboot` 绕回 UniXNU |
| `fastboot flash` 显示 `????????????` | USB 会话坏；拔插线或重启手机进 fastboot |
| 抓控制台 `Permission denied: /dev/ttyACM0` | tty 属 root:dialout；用 `sudo -n python3 scripts/capture-usb-console.py` |
| `podman` 报 `newuidmap` / `/run/user/1000/libpod` 只读 | `export XDG_RUNTIME_DIR=/tmp/xdgrt` |
| mtools 命令永久挂起 | `/dev/tty` 交互 + SIGTTIN；脚本已用 `setsid` + `< /dev/null` + `-o` |
| payload 补丁报 "padding ends at 0x..." | stage-3 洞 + panic screen 超出内核 padding；二分时关 panic_screen |
| 内核停在 lockdown、IRQ=0 | GIC 未预配置（DarwinBoot PrepareGic，本仓库已含） |
| 屏幕只有色带没日志 | boot-progress 未编入或 `boot-progress=2` 参数缺失 |
| 面板只有顶部几行、下面被大字覆盖 | 这是 panic screen；第 0..19 行是面板，第 20 行起被清屏 |
| `pc=0 / lr=...3ef87c` 取指异常 | `_sleh_irq` 的 CPU IRQ handler 为 NULL；不要提前使能 timer PPI |
| "CPU configuration in device tree is invalid" | AppleARMCPU 抢到 cpu nub；`sheng-smp=1` + make-uboot-payload 的改名 |
| `no external trust caches found` 后 launchd 被 SIGKILL | payload 漏了 `--trust-cache`；重做 payload 再刷 |

## 8. 仓库结构（本仓库）

```
board.toml                        # 板级配置（内核参数、payload、ADT 参数、探针）
porting.toml                      # 机器校验的移植记录（scripts/porting-record.py check）
configs/sirius_defconfig          # 内核配置（CONFIG_XNU_* → 构建脚本的 XNU_* 变量）
drivers/ArmCpu/                   # 覆盖版 ArmCpu（probe 计数器 + timer INTID 参数化）
drivers/DarwinBoot/               # 覆盖版 XNU 加载器（GIC 预配置）
package/                          # kit：boot-progress、panic screen、USB 环、eMMC 驱动、探针
patches/xnu/                      # 板级 XNU 补丁（series 里按顺序登记）
efi/                              # EDK II 包（UniXNU.efi 与 DXE 驱动，vendor 自框架）
uboot/                            # U-Boot submodule + unixnu-sirius.env + build.sh
containers/xnu-linux/             # 内核构建容器（Containerfile）
scripts/                          # 板级构建配方、payload 终装、刷机/抓日志、探针补丁
docs/                             # 本文档、linux-kernel-build.md、evidence/、archive/
```

## 9. 提交历史

以 `git log` 为准；逐阶段记录在 `docs/archive/HANDOFF-stages.zh-CN.md`，早期设计笔记在
`docs/archive/HANDOFF-design-notes.zh-CN.md`。
