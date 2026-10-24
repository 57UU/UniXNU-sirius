# UniXNU-sirius

[UniXNU](https://github.com/UniXNU/UniXNU) device support for the **Xiaomi Mi 8 SE**
(sirius, Snapdragon 710): the first-boot bring-up of iOS/iPadOS on this phone.

## What is different about this port

The phone runs [sirius-distro](https://github.com/57UU/sirius-distro) (mainline Linux) and its
boot partition carries a **U-Boot 2026.07 with full UEFI support** (`CONFIG_EFI_LOADER`,
`CMD_BOOTEFI_BOOTMGR`, GOP via vidconsole, EFI block volumes over FAT/ext4). That U-Boot
replaces the usual Mu-Silicium firmware step:

```
ABL (boot.img = U-Boot)  ->  U-Boot UEFI
                            \\EFI\\BOOT\\BOOTAA64.EFI   UniXNU.efi   (from a FAT partition)
                            \\EFI\\UniXNU\\unixnu.toml  + Payloads\\kernel.macho(+.adt)
                            Payloads\\DarwinBoot.efi    XNU loader (a [[driver]] file)
                            -> XNU 23.6.0 (4 KiB BCM2837 personality, prelinked 21H461 kexts)
```

No `unixnu build firmware` and no boot image: the payload FAT volume is flashed to the unused
**vendor partition (`mmcblk0p80`, 3 GiB)** and selected from the U-Boot boot menu.

## Status

| Stage | State |
| --- | --- |
| Stage 1: UEFI (U-Boot) | working (sirius-distro, `fastboot flash boot` + the marker-file boot) |
| Stage 3: payload volume | working (p80 vendor FAT, `fastboot flash vendor`) |
| Stage 4: XNU entry | working (green/blue/magenta entry-probe bands on hardware) |
| Stage 5: platform expert | working (MMU on, 19.2 MHz timer, PSCI reset proven by the stage-3 reset loop) |
| Stage 6: console | working (boot-progress panel: live kernel log, NVRAM proxy, memory map on hardware) |
| Stage 7: IOKit | in progress. The boot-progress TOPO row confirms the kernel topology (`boot=0 max=7 nums=01234567`). Native ArmCpu has not yet claimed the `/cpus` nubs: the kernelcache AppleARMCPU takes them and panics in its `_validateConfiguration()` (it requires `/cpus/max_cpus`). The SoC virtual timer is PPI 3 / INTID 19 (not Apple's 27); enabling it in DarwinBoot's early GIC pre-config makes the timer fire before ArmCpu installs its IRQ handler (pc=0 abort in `sleh_irq`), so the next step is to stop pre-enabling that PPI and re-read the probe row |
| Stage 8: root device | pending (eMMC board: no storage driver yet; the expected first stop) |

The full context, the build-deploy-test loop, the solved problems and the road map are in
[docs/HANDOFF.zh-CN.md](docs/HANDOFF.zh-CN.md) (Chinese).

## Layout

| Path | Contents |
| --- | --- |
| `board.toml` | board description for the `unixnu` build driver |
| `configs/sirius_defconfig` | kernel configuration of the first-console image |
| `scripts/make-sirius-adt.py` | the Apple-format device tree (SDM710 values from `sdm670.dtsi`) |
| `scripts/make-uboot-payload.py` | adds UniXNU.efi + DarwinBoot.efi to the payload volume |

## Hardware values (from `sdm670.dtsi` / the phone)

| Value | sirius |
| --- | --- |
| DRAM | `0x80000000`, `0x17d6c0000` usable (6 GiB model: 2 GiB + 0xfd6c0000 at 0x100000000) |
| GIC-500 | GICD `0x17a00000`/0x10000, GICR `0x17a60000`/0x100000 (stride 0x20000) |
| Timer | 19.2 MHz (Qualcomm XO); EL1 virtual timer = GIC PPI 3 / INTID 19 (Apple uses PPI 11 / INTID 27) |
| CPUs | 8x Kryo 360 at MPIDR 0x000-0x700; cpu0-5 = A55, cpu6-7 = A75; boot CPU cpu0 |
| PSCI | smc |
| Storage | eMMC (this unit); no native storage driver yet |
| Display | 1080x2244 DSI (U-Boot vidconsole keeps a 1080x2240 splash framebuffer at 0x9c000000) |

## Building

The kernel step needs macOS + Xcode (the GitHub Actions workflow in this repository runs it);
everything else builds on Linux:

```sh
git clone --recurse-submodules https://github.com/UniXNU/UniXNU
git clone https://github.com/57UU/UniXNU-sirius
cd UniXNU
./unixnu defconfig sirius_defconfig -D ../UniXNU-sirius
./unixnu build kernel            # macOS (or the CI workflow of this repository)
./unixnu build package           # UniXNU.efi (Linux OK)
bash scripts/build-darwinboot.sh # DarwinBoot.efi (Linux OK)
./unixnu build payload           # the FAT volume (Linux OK)
python3 ../UniXNU-sirius/scripts/make-uboot-payload.py \
  --payload out/sirius/payload/payload.img \
  --unixnu-efi package/Build/Out/AARCH64-DEBUG/UniXNU.efi \
  --darwinboot-efi build/DarwinBoot/DEBUG_CLANGPDB/AARCH64/DarwinBoot.efi
```

## License

BSD-2-Clause-Patent, as UniXNU.
