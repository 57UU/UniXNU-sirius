# sirius 阶段日志 —— 实测值、错误判断与坑（2026-10-25 → 10-28）

当前状态、构建回路和下一步在 `docs/HANDOFF.zh-CN.md`；机器校验的事实摘要在 `porting.toml`。

本文件**只保留 git 还原不了的东西**：真机上的实测数值、后来被推翻的判断（连同它为什么当时看起来
成立）、以及"再踩一次就挂机"的操作。**逐阶段叙述已删除**——每轮改了什么看 `git log`，
每轮为什么这么改看提交里的 `porting.toml` findings。要恢复旧版全文：
`git show <commit>:docs/archive/HANDOFF-stages.zh-CN.md`。

---

## 1. 已作废的判断（这些结论当时都有"证据"，但都是错的）

| 当时的结论 | 真正的答案 | 为什么当时看着像真的 |
| --- | --- | --- |
| exec 之后的停顿在存储栈 / 共享区没建立 | 是 **PAN**：`CONFIG_XNU_NATIVE_PAN=n` 让内核再也清不掉 PAN，任何对用户页的内核访问都吃 permission fault | fault 计数冻结在某个 LBA、驱动没有在途传输，读起来像"存储读完了在等别的东西" |
| **PAN 不是原因** | 错。当时在**内核线程**里读 `PSTATE.PAN` 读回 0，是因为本驱动自己的 PAN 报告把它清掉了 | 采样点不在 fault 现场；驱动清 PAN 的副作用没被发现 |
| `pc=0, lr=...3ef87c` 是 pmap 的问题 | 是 `_sleh_irq` 的 per-CPU handler 为 NULL：DarwinBoot 提前使能了 timer PPI，timer 在 ArmCpu 装好 handler 之前就开火 | 取指异常指向内核地址，很像页表问题 |
| 信任链查询时的 "Taking non-sleepable RW lock" panic 是 pmap 锁 | 是 `_trust_cache_rt_lock`：`trust_cache_runtime_init()` 没编进这个 build，锁字全零（`can_sleep == false`） | 同一条 panic 在 `lock_rw.c` 里有 5 处，只改一处的打印会看不出变化 |
| 根卷是 SHNGROOT 清单，不是 APFS（第九阶段） | 清单只是**驱动侧**的窗口描述；窗口里是 IPSW 原样的 APFS 镜像，APFS 能挂（第十阶段自我纠正） | 把"清单的消费者是块驱动"读成了"根不是 APFS" |
| AMFI 的 exec 策略只能靠 boot arg 绕过（`sirius-skip-cs=1`） | 原始 trust-cache bundle 就够了，AMFI 自己放行，`sirius-skip-cs` 已退役 | 没有 bundle 时 AMFI 确实一律拒绝（namespace 9 / code 8） |
| dyld 缓存要靠 `/chosen/asmb`（lp-sip0/lp-sip1）放开 SIP | 本 build `CONFIG_CSR=0`（`config_csr.h` 为 `#define NCONFIG_CSR 0`），`vm_unix.c` 走的是"缓存在进程根挂载上"的分支，那条路不适用 | ios-sweet（iOS 16，`CONFIG_CSR≠0`）的记录里确实这么做 |
| 400 kHz 是停顿的原因 | 只是慢（约 550 blocks/s，4 位总线的理论上限），停顿另有原因 | 停顿的时刻驱动没有传输在途，容易归因到最慢的部件 |
| "静默挂起"（两次误判） | 一次是 `IOLog` 到不了 USB 环（console 在 `bsd_init` 前就不再喂环）；一次是 `emmc_log` 被 clang 内联后 object 里丢了字面量 | 两次都是"代码在跑、日志是空的"，表现完全一样 |

---

## 2. 实测值（真机读出来的，改代码请对照）

### eMMC / SDCC1 —— 缺一不可的五个条件

1. **RCG2 编码**：divider 字段放 `div * 2 - 1`，`n != m` 时置 DUAL_EDGE。
   400 kHz = CXO/div12/m1/n4 → `CFG 0x2017`；25 MHz = GPLL0_EVEN/div6/m1/n2 → `CFG 0x260B`。
2. **1.8 V I/O**（板子的 vqmmc）。
3. **中断/错误映射是 32 位的**，在 `0x30/0x32/0x36`（按 SDHCI 的 16 位布局写永远收不到完成）。
4. **ADMA2 描述符地址在 `0x58/0x5c`**，不是 `0x00`。
5. **cache maintenance**：引擎读内存而不是 CPU cache，必须 `flush_dcache64`。

其它：`SDHCI_TRANSFER_MODE` 写 `0x13` 读回 `0x33`；SMMU 在启动链里是关的
（`scr0=0x00230406`，bit0=0），驱动只读不写。

### 分区与镜像集

- 整盘 `0x747BFDF` 个 512 字节扇区；GPT `entries_lba=2, count=84, size=128`，81 个分区。
  boot `1179648..1310719`，vendor(p80) `11010048..17301503`，userdata(p81) `17301504..122142686`。
- SHNGROOT manifest 在 userdata LBA `17301504`（4 KiB，1 MiB 对齐）；窗口：
  OS `17303552 + 11870275`，SystemOS `29175808 + 6451200`。
- 卷 UUID：OS 根 `04769D26-2CA0-4825-B443-E2E5F88300F2`；
  SystemOS cryptex `8846FE78-2910-4297-AF97-D59BB6F8FC16`（内含
  `System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64` + 53 个子缓存，顶层只有 `System` 和 `usr`）。

### USB（DWC3）

物理端点号 `phys = 2 × USB 端点号 + 方向`：描述符写 `0x01`(EP1 OUT)→2、`0x81`(EP1 IN)→3、
`0x82`(EP2 IN)→5。当时为了"让描述符与物理号一致"改成 `0x83/0x02/0x85`，对应物理 7/4/11
——**这块核上不存在**，于是 `DALEPENA`、`DEPCMD`、TRB 全部正常但永远没有完成事件（`tx_event=0`）。

### PAN

`ID_AA64MMFR1_EL1 = 0x10212122`（PAN 字段 `0b0010` = FEAT_PAN2，每次进 EL1 都置 PAN，
与 `SCTLR_EL1.SPAN` 无关，实测 `SCTLR_EL1 = 0x3454593d`）。故障时 `SPSR_EL1 = 0x20400204`、
`TTBR0_EL1 == pmap->ttep == 0xa43f2800`、`ESR = 0x9600004f`（level-3 permission）、
`AT S1E1W` 能翻译（AT 不受 PAN 约束）。PAN=n 时 fault 数 6.27e7 次/分钟 / 9.22e7 次；
PAN=y 后降到 122 次，launchd 入口 `0x1005dee50`，24 条 syscall。

### 吞吐

400 kHz、4 位总线：约 550 blocks/s（0x3FC → 0x84A 两秒内）。25 MHz 已验证
（`EMMC clock25=ok`，读回 GPT 签名）。

---

## 3. 危险记录与硬规则

- **不要盲写 SMMU。** 向 apps SMMU 写 SMR/S2CR（SID `0x140`）会让整机在启动中失去 USB、
  无任何输出，只能强制重启。要建映射必须按 CBAR/CBA2R/SCTLR + 页表走，且在该 stream 未使能时写；
  只读校验（`emmc_smmu_probe()`）是安全的。
- **探针必须在 USB 控制台之前跑完，且任何等待都要有硬上限（建议 ≤200 ms）。** 一次
  `HOST_CONTROL` 改成 1 位总线就让整机再没枚举（`device not accepting address, error -62`），
  唯一的日志通道随之消失。
- **一次刷机只改一个变量**，改完先确认能开机、能出日志；写 SMMU / 改总线宽度 / 改时钟之前，
  先确认"能启动"。
- **不要提前使能 timer PPI（INTID 19）**：它会在 ArmCpu 装好 handler 之前开火，
  表现是 `pc=0 / lr=...3ef87c`。DarwinBoot 只使能 SGIs 0..15。
- **PAN 的报告只能读，不能清。** 清掉会让 `machine_switch_context` panic
  （`context switch with PAN disabled` @`pcb.c`）。
- **传输不能并发，也不能用会睡的锁。** APFS 挂多卷会并发读，而 staging buffer / ADMA 描述符表
  是全局唯一的；用 `IOLock` 会撞上"不可睡 rw 锁 + 抢占"panic（读路径可能在不可睡上下文）。
  现在是原子标志 + `IODelay` 单飞。
- **`doAsyncReadWrite` 默认内联完成。** 在别的线程里完成会让这套栈以同样方式死锁
  （读和完成都冻住、无错误、drain 睡死）；`thread_call` 只有一个 pending 槽，
  `thread_call_enter1` 会覆盖参数从而丢请求——所以请求走软件队列 + 一个 drain。
- **`stop()` 只能丢指针**，不要 `terminate()`+`release()` 仍注册在表里的块设备，
  否则 registry corruption。
- **panic 文本要镜像进 USB 环**（心跳线程做），否则 panic 现场只剩拍照。
- **`emmc_log` 必须 `noinline`**，否则字面量在 object 里消失，纯文本行全部丢失。

---

## 4. 调试设施的经验

- `IOLog` / `printf` 到不了 USB 环（console 在 `bsd_init` 之前就停止喂环），驱动和探针都直写
  `sirius_usb_log_char()`。
- 环只交"连接时刻"的内容，但**主机发任意字节会让驱动重放整个环**——`capture-usb-console.py`
  就是靠定时 poke 拿到完整启动日志的。
- 首次推流有效形态：内联轮询 ≤45 s、**排空后立刻 break**、首次延迟 3000 ms。
  试过不 work 并已回退：120 s 不 break、8 s 不 break、`SET_CONTROL_LINE_STATE`(DTR) 门控
  （主机 DTR 没到达）。机理没完全清楚，**在弄清前不要动这段时序**。
- 面板冻住 ≠ 卡死：面板靠 timer 中断重绘，timer 还没开时最后一帧会留在屏幕上。判断靠 checkpoint
  （`armcpu-panic=N`）和 panic 时的强制重绘。
- `tx_event`/`rx_event` 这类事件字比任何推断都直接：`0` 就是"该端点从未完成过传输"，
  与"命令被接受、地址正确"同时出现时几乎一定是端点号/映射错，而不是时钟或 DMA。

---

## 5. 证据文件

| 文件 | 内容 |
| --- | --- |
| `docs/evidence/sirius-usb-console-20261026-1605.log` | 第一次从 `/dev/ttyACM0` 直接抓到的内核日志（3813 字节），CDC-ACM 打通 |
| `docs/evidence/sirius-pan-fault-20261027-2106.log` | PAN=n 时的 fault 现场（SPSR/SCTLR/TTBR0/ESR + 计数） |
| `docs/evidence/sirius-exec-after-pan-20261027-2125.log` | PAN=y 之后 exec 完成、launchd 进入用户态、24 条 syscall |
| `docs/evidence/sirius-dyld-cache-absent-20261027-2155.log` | 缓存路径全 ENOENT，`/usr/lib/dyld` 与 `/sbin/launchd` 正常 |
| `docs/evidence/sirius-launchd-stop-20261028-1350.log` | union 之前的 launchd 停顿（摘录 + kernel sha256） |
