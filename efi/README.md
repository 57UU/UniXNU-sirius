# EFI applications (`UniXNU.efi`, `DarwinBoot.efi`, the DXE drivers)

This is a copy of UniXNU's EDK II package, taken so that this repository can build
the whole boot chain on its own instead of relying on a separate checkout of the
framework.

| | |
| --- | --- |
| upstream | `github.com/UniXNU/UniXNU`, directory `package/` |
| taken at | `f85b5a6` |
| contents | `UniXNUPkg/` (the `UniXNU.efi` loader, the DXE drivers the loader starts, `DarwinBoot` as a `[[driver]]`), `Scripts/`, `build.sh`, `compile_flags.txt`, `LICENSE` |

## What the payload needs from here

The FAT payload volume (`vendor` partition, p80) is built by the framework's
`unixnu build payload` from three pieces: the prelinked kernel, this EDK II
package's `UniXNU.efi`, and `DarwinBoot.efi`. `scripts/make-uboot-payload.py` then
assembles them with the `EFI/UniXNU/BOOTUNIXNU` marker that U-Boot's `unixnu_check`
looks for.

`DarwinBoot.efi` is also forked in this repository (`drivers/DarwinBoot`, with the
GIC pre-configuration this board needs); build the payload with the fork, not with
the copy under `efi/UniXNUPkg`.

## Building

The package needs an EDK II checkout and a host cross toolchain; the framework's
`unixnu` CLI drives both (`package/build.sh` is its entry point). Keeping this copy
in step with upstream is a manual `cp` from the framework's `package/` — the file
list is small on purpose, so a diff is readable.

If that drift becomes a problem, the alternative is to make the framework a
submodule here and drop this copy; it is vendored today because the fork commits in
`drivers/` and the EDK II package track each other closely and a readable diff beats
a second checkout.
