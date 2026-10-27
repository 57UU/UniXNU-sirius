# UniXNU-sirius

[UniXNU](https://github.com/UniXNU/UniXNU) device support for the **Xiaomi Mi 8 SE**
(sirius, Snapdragon 710 / SDM710): bringing the source XNU kernel and an iPadOS userland up on
this phone.

Format 2 port (`schema_version = 2`): the board selects shared ARM64, SoC and OS profiles and
supplies only its own wiring. The shared SDM710 layer is
[UniXNU-sdm710-common](https://github.com/57UU/UniXNU-sdm710-common).

> Rewritten from the legacy port. The previous layout and its full history are on the
> [`legacy`](../../tree/legacy) branch of this repository, which is also where the original
> `docs/HANDOFF.zh-CN.md` and the last working images live.

## What is different about this port

The phone runs [sirius-distro](https://github.com/57UU/sirius-distro) (mainline Linux) and its
boot partition carries a **U-Boot 2026.07 with full UEFI support** (`CONFIG_EFI_LOADER`,
`CMD_BOOTEFI_BOOTMGR`, GOP via vidconsole, EFI block volumes over FAT/ext4). That U-Boot
replaces the usual Mu-Silicium firmware step, so this port has neither `unixnu build firmware`
nor a boot image:

```
ABL (boot.img = U-Boot)  ->  U-Boot UEFI
                            \EFI\BOOT\BOOTAA64.EFI      UniXNU.efi   (from the vendor FAT volume)
                            \EFI\UniXNU\unixnu.toml     + Payloads\kernel.macho (+.adt)
                            Payloads\DarwinBoot.efi     XNU loader (a [[driver]] of unixnu.toml)
                            -> XNU 23.6.0 (4 KiB BCM2837 personality, prelinked 21H461 kexts)
```

The payload FAT volume is flashed to the unused **vendor partition (`mmcblk0p80`, 3 GiB)** and
selected from the U-Boot boot menu.

## Status

| Stage | State |
| --- | --- |
| UEFI (U-Boot) | working: `fastboot flash boot` the U-Boot image, `fastboot flash vendor` the payload, marker-file boot |
| Payload volume | working: p80 FAT with `\EFI\UniXNU\BOOTUNIXNU` |
| XNU entry | working: green/blue/magenta entry-probe bands on hardware |
| Platform expert | working: MMU on, 19.2 MHz timer, PSCI reset proven by the reset loop |
| Console | working: boot-progress panel with the live kernel log, and a CDC-ACM USB console whose ring also carries the kernel's own `printf` output and any panic message |
| IOKit | working: `sheng-smp=1` makes the native ArmCpu claim every `/cpus` nub and the board publishes the `ApplePMGRNub` AMFI looks for, so the prelinked kexts start |
| Storage (eMMC) | working: SDCC1 at 400 kHz, SHNGROOT windows of `[userdata]` published as block devices, whole-disk media of 0x747BFDF sectors; IOStorageFamily parses the GPT through the driver |
| Root filesystem | working: APFS mounts the OS volume (`disk2s1`), `bsd_init` completes and the sealed-root answer is recorded instead of panicking |
| init / launchd | working: `execve` of `/sbin/launchd` completes (`execve activate 0`); the exec policy answer is recorded with `sirius-skip-cs=1` until the trust cache is supplied |
| Userland | **next**: after the successful exec the kernel stops asking the driver for blocks, so the remaining wait is past storage; the eMMC clock also needs to leave 400 kHz |

Where it stops now: `execve` returns 0 for launchd, the driver then sees no further
requests, and nothing else is printed. The eMMC clock is still the identification
rate (about 550 blocks/s), which is a speed problem rather than the cause of that
wait. The proper fix for the exec policy is the original trust-cache bundle
(`build-original-system-trust.py` + `build payload --trust-cache`), which also
retires `sirius-skip-cs=1`.

The full history, the solved problems and the road map are in `docs/HANDOFF.zh-CN.md`; the
machine-checked version of the same facts is `porting.toml`.

## Layout

| Path | Contents |
| --- | --- |
| `board.toml` | format 2 board description: profiles, components, payload, Apple device-tree properties |
| `feeds.toml` | the pinned SDM710 SoC source |
| `dts/sirius.dts` | board hardware description (memory, eight cores, PSCI); includes the SoC DTSI |
| `configs/sirius_defconfig` | kernel configuration of the first-console image |
| `package/` | board services built into the kernel: boot progress, panic screen, probes (`kits/sirius` in the tree) |
| `drivers/ArmCpu`, `drivers/DarwinBoot` | board forks of the two components this port changes |
| `uboot/` | U-Boot as a pinned submodule (`uboot/src`, branch `sirius/v2.0`), the environment file that adds the UniXNU boot chain, and `build.sh` for `boot-unixnu.img` |
| `efi/` | the EDK II package (`UniXNUPkg`, `Scripts`) this port builds `UniXNU.efi` and the DXE drivers from, vendored so the repository can build the whole chain |
| `scripts/` | board build recipes, the U-Boot payload finisher and the entry-probe patcher |
| `porting.toml` | per-area porting record (status, evidence, limits, next action) |
| `docs/HANDOFF.zh-CN.md` | the maintainer handoff: architecture, build-deploy loop, solved problems |

## Building

The kernel step needs macOS + Xcode (the `kernel` workflow in this repository runs it) **or** the
Linux toolchain inside the Podman image of [`containers/xnu-linux/`](containers/xnu-linux/)
(`./scripts/xnu-container.sh build-image`, then `... run bash -lc './unixnu build kernel -D ../sirius'`);
everything else builds on Linux against the same UniXNU checkout.

```sh
git clone --recurse-submodules https://github.com/UniXNU/UniXNU
git clone https://github.com/57UU/UniXNU-sirius
cd UniXNU
./unixnu defconfig sirius_defconfig -D ../UniXNU-sirius
./unixnu fetch
./unixnu status --json
./unixnu build kernel            # macOS (or the CI workflow of this repository)
./unixnu build package           # UniXNU.efi (Linux OK)
bash scripts/build-darwinboot.sh # DarwinBoot.efi (Linux OK), from the assembled tree
./unixnu build payload           # the FAT volume (Linux OK)
python3 ../UniXNU-sirius/scripts/make-uboot-payload.py \
  --payload out/sirius/ipados-17.7.11-21H461/sirius/payload/payload.img \
  --unixnu-efi out/sirius/ipados-17.7.11-21H461/sirius/package/Build/Out/AARCH64-DEBUG/UniXNU.efi \
  --darwinboot-efi build/DarwinBoot/DEBUG_CLANGPDB/AARCH64/DarwinBoot.efi
```

Outputs live under `out/sirius/ipados-17.7.11-21H461/sirius/`. `unixnu status --json` prints the
resolved profiles, revisions and patch order.

## Hardware values

From the mainline `sdm670.dtsi` (SDM710 is a superset), the vendor tree and the phone:

| Value | sirius |
| --- | --- |
| DRAM | `0x80000000`, `0x17d6c0000` usable (6 GiB model: 2 GiB + `0xfd6c0000` at `0x100000000`) |
| GIC-500 | GICD `0x17a00000`/0x10000, GICR `0x17a60000`/0x100000 (stride 0x20000) |
| Timer | 19.2 MHz (Qualcomm XO); EL1 virtual timer = GIC PPI 3 / **INTID 19** (Apple uses PPI 11 / INTID 27) |
| CPUs | 8x Kryo 360 at MPIDR `0x000`–`0x700`; cpu0-5 = A55, cpu6-7 = A75; boot CPU cpu0 |
| PSCI | `smc` |
| Storage | eMMC (this unit); no native storage driver yet |
| Display | 1080x2244 DSI (U-Boot keeps a 1080x2240 splash framebuffer at `0x9c000000`) |

## Known deviations from upstream UniXNU

1. **No firmware step.** `[firmware]` is absent and the payload is the artifact; see above.
2. **`scripts/build-xnu-4k-baseline.sh` is a copy of the UniXNU script with one change**: the
   boot-progress interlock requires only the source crypto provider, because this board is eMMC
   and has no UFS/owner-root/Virtio stack. Re-base it when UniXNU changes the script.
3. **Board forks of two components.** `drivers/ArmCpu` and `drivers/DarwinBoot` are device-local
   copies; upstream sees them only through this repository.

## Links

- [Device record](https://unixnu.com/devices/sirius)
- [Port a device](https://unixnu.com/porting/), [device repository](https://unixnu.com/porting/device-repository)
- [Prelink](https://unixnu.com/concepts/prelink), [boot chain](https://unixnu.com/concepts/boot-chain)

## License

BSD-2-Clause-Patent, as UniXNU.
