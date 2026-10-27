# U-Boot for UniXNU (`boot-unixnu.img`)

The U-Boot **source is not modified**. This port needs one thing from U-Boot: an
`mmc`/`load`/`bootefi` chain that can start `/EFI/BOOT/BOOTAA64.EFI` from the
vendor partition (p80), opt-in per boot through a marker file. U-Boot's default
environment is compiled into the image, so that chain lives in
[`unixnu-sirius.env`](unixnu-sirius.env) instead of a patch:

```
unixnu_boot=load mmc 0:50 ${kernel_addr_r} /EFI/BOOT/BOOTAA64.EFI; bootefi ${kernel_addr_r}:${filesize}
unixnu_check=if load mmc 0:50 ${scriptaddr} /EFI/UniXNU/BOOTUNIXNU; then run unixnu_boot; fi
bootmenu_2=Boot UniXNU (p80 vendor)=run unixnu_boot
bootcmd=run check_bcb; run unixnu_check; run do_boot
```

`unixnu-sirius.env` is the SoC board's stock `sirius.env` plus exactly those lines
(and the bootmenu renumbering they need); diff the two to see it.

## Build

```bash
git clone --recurse-submodules https://github.com/57UU/sirius-distro   # U-Boot lives in uboot/
cd sirius-distro
ENV_FILE=$PWD/../UniXNU-sirius/uboot/unixnu-sirius.env ./uboot/sirius-build.sh
cp uboot/build/boot.img boot-unixnu.img
fastboot flash boot boot-unixnu.img
```

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
