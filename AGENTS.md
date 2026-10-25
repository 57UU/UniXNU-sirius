# Device agent guide

This repository owns the sirius board: its hardware description, the selection of shared
components, the payload policy and the porting record. It does **not** own the GIC/counter/PSCI
facts (that is [UniXNU-sdm710-common](https://github.com/57UU/UniXNU-sdm710-common)), the XNU
source or the OS profile (those are UniXNU), or the U-Boot firmware (that is sirius-distro).

## Read first

Read `README.md`, `docs/HANDOFF.zh-CN.md` and these UniXNU pages before changing the port:

| Task | Required reading |
| --- | --- |
| Every port | [Porting](https://unixnu.com/porting/), [device repository](https://unixnu.com/porting/device-repository), [component catalog](https://unixnu.com/reference/components) |
| Hardware | [Device tree](https://unixnu.com/porting/device-tree), the sdm710 SoC `README.md`, `soc.toml` and `dts/sdm710.dtsi` |
| Kernel | [Source and patches](https://unixnu.com/xnu/source-and-patches), [patch reference](https://unixnu.com/xnu/patch-reference), the selected OS profile |
| Images | [Prelink](https://unixnu.com/concepts/prelink), [payload](https://unixnu.com/concepts/payload), [boot chain](https://unixnu.com/concepts/boot-chain) |
| Record | [Porting records](https://unixnu.com/porting/records) — `porting.toml` is machine-checked |

## Work

Everything about this port is already implemented once in the `legacy` branch. Prefer reading
that history over re-deriving a value from scratch.

1. A shared SoC value (GIC base, counter, PSCI, timer INTID) goes to the SoC repository, not here.
2. A board value (memory, CPU selection, payload policy, Apple device-tree deviations) goes in
   `board.toml` or `dts/sirius.dts`.
3. A change to XNU itself belongs in a series: the ARM64/OS series of UniXNU when it is general,
   `patches/xnu/series` of this repository when it is board specific.
4. A board fork of a component stays under `drivers/<ID>` and is listed in `board.toml`
   `[components] include` by its canonical ID.
5. Update the affected `porting.toml` area in the same change: findings or evidence, with the
   image hashes and the checkpoint that was observed.

## Checks

Run from a UniXNU checkout:

```sh
./unixnu defconfig sirius_defconfig -D ../UniXNU-sirius
./unixnu fetch
./unixnu status --json
./unixnu tree
python3 scripts/porting-record.py check --device ../UniXNU-sirius
```

The Device documentation workflow runs the record check on every push.

## Hardware work

There is one bootable target and it is a phone: every experiment is a flash and a reboot.

- Kernel: `macOS + Xcode only`, or the `kernel` workflow of this repository.
- Payload: Linux is enough; `make-uboot-payload.py` finishes the volume.
- Evidence: the boot-progress panel is the log. Photograph the rows and record the kernel,
  DarwinBoot and payload hashes with the observation.

Keep `armcpu-timer-intid=19` and `sheng-smp=1` in the command line; both encode hardware facts
(see `docs/HANDOFF.zh-CN.md`). Changing the command line requires rebuilding the payload and
re-flashing the vendor partition.
