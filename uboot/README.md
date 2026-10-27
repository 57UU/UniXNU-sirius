# U-Boot for UniXNU (`boot-unixnu.img`)

The U-Boot **source is not modified**. This port needs one thing from U-Boot: an
`mmc`/`load`/`bootefi` chain that can start `/EFI/BOOT/BOOTAA64.EFI` from the
vendor partition (p80), opt-in per boot through a marker file. U-Boot's default
environment is compiled into the image, so that chain lives in
[`unixnu-sirius.env`](unixnu-sirius.env) instead of a patch:

```
unixnu_arm=if bcb load 0 misc; then bcb set command uboot-fastboot; bcb store; fi
unixnu_boot=run unixnu_arm; load mmc 0:50 ${kernel_addr_r} /EFI/BOOT/BOOTAA64.EFI; bootefi ${kernel_addr_r}:${filesize}
unixnu_check=if load mmc 0:50 ${scriptaddr} /EFI/UniXNU/BOOTUNIXNU; then run unixnu_boot; fi
bootmenu_2=Boot UniXNU (p80 vendor)=run unixnu_boot
bootcmd=run check_bcb; run unixnu_check; run do_boot
```

`unixnu-sirius.env` is the SoC board's stock `sirius.env` plus exactly those lines
(and the bootmenu renumbering they need); diff the two to see it.

## Fastboot after a hang (work in progress)

A bring-up kernel that hangs cannot reset itself, so a test used to end with the
Vol-Down + Power chord. Two ways to get back to a *flashing* fastboot are known, and
neither is relied on yet:

- **BCB arming works for entering, not for flashing.** Writing the command
  `uboot-fastboot` into the Android bootloader control block (`bcb load 0 misc;
  bcb set command uboot-fastboot; bcb store`) before booting UniXNU makes the next
  start run U-Boot's own `fastboot usb 0`; that was verified on the phone. U-Boot's
  fastboot answers `getvar` but a partition flash hangs, so it is not a place you can
  flash from. The stock `check_bcb` line above implements this and is left in place;
  the arming line is commented out in `unixnu-sirius.env` until flashing works.
- **The PMIC spare is what ABL reads.** Linux's `reboot bootloader` only writes
  `mode-bootloader` into `PON_SOFT_RB_SPARE` (PM660 SPMI PID 8 offset 0x8f, register
  `0x88f` in U-Boot's encoding, value `0x04`) and lets the reset happen; the boot chain
  then lands in ABL's own fastboot, which does flash. Doing that from U-Boot needs
  `CONFIG_CMD_PMIC` (already known to build) and a verified `pmic` device name, which
  has not been proven on hardware yet.

The fastboot flashing hang itself is understood: `fastboot_tx_write()` in
`drivers/usb/gadget/f_fastboot.c` cancels the IN request before queueing it again, and
this U-Boot's dwc3 gadget has the older ENDTRANSFER/dequeue path, so the cancel can
leave a TRB the controller still owns. The local build here carries the one-line fix
(drop that dequeue); it needs a device test before it is committed to the U-Boot
branch.

## Build

```bash
git clone --recurse-submodules https://github.com/57UU/sirius-distro   # U-Boot lives in uboot/
cd sirius-distro
ENV_FILE=$PWD/../UniXNU-sirius/uboot/unixnu-sirius.env ./uboot/sirius-build.sh
cp uboot/build/boot.img boot-unixnu.img
fastboot flash boot boot-unixnu.img
```

On Fedora 44 the host tools need `openssl/engine.h`, which OpenSSL 3.5 dropped while
libcrypto still exports the API: pass a shim include directory through
`HOSTCFLAGS=-I<dir>` (a three-prototype header is enough, the tools are never used
to sign anything here).

`sirius-build.sh` passes `ENV_FILE` as `CONFIG_ENV_DEFAULT_ENV_TEXT_FILE`, so the
submodule stays clean (`build/sirius-distro`… see `uboot/sirius-build.sh` for the
output path it uses).

## Two traps

- **`mmc 0:50` is hexadecimal.** `0x50` = 80 = the vendor partition. Writing
  `0:80` is read as `0x80` = 128 and the command fails with
  `Invalid partition 128`.
- **Do not use `bootflow scan`.** It stops after the first 30 partitions of each
  device, and p80 is beyond that on this phone; `load`/`sysboot` with the partition
  spelled out is the reliable form.

## Recovering Linux

The stock `sirius-distro` build (`build/boot.img`) ignores the marker and boots
Linux:

```bash
fastboot flash boot ~/Projects/sirius-distro/build/boot.img
```

`make-uboot-payload.py --marker` is what writes `/EFI/UniXNU/BOOTUNIXNU` into the
payload volume this `unixnu_check` looks for.
