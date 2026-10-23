# Building the kernel on Linux (WSL)

The kernel step is designed for macOS + Xcode (see the workflow in `.github/workflows/kernel.yml`).
This note records what a Linux host build needs, from the attempt of 2026-10-24.

## What works on Linux already

- `unixnu build package` (UniXNU.efi), `scripts/build-darwinboot.sh` (DarwinBoot.efi),
  `unixnu build payload` (the FAT volume; llvm-nm/ld.lld/llvm-objcopy/llvm-size needed).
- `ld64.lld` links the kernel: translate `-Wl,-ios_version_min,17.0` to
  `-Wl,-platform_version,ios,17.0,17.0`.

## What is missing: the host tools

| Tool | Why | Status |
| --- | --- | --- |
| `xcrun` | XNU makedefs call `/usr/bin/xcrun` (absolute!) for SDK resolution and `-find TOOL` | a PATH shim works for `-find`/`--show-sdk-path`, but the absolute path needs a symlink |
| `sysctl -n hw.memsize` | `makedefs/MakeInc.top` (`/usr/sbin/sysctl`, absolute) | a wrapper script works |
| `ditto` | libplatform header install | a `cp -a` wrapper works |
| `mig` / `migcom` | `.defs` → RPC stubs (`xcrun -sdk iphoneos -find mig`) | **not ported**: migcom (bootstrap_cmds, flex/bison/C) builds until the Mach headers it includes hit glibc/BSD typedef conflicts (`__int64_t` long vs long long and friends) |

## The pragmatic local-iteration route

The mig outputs are deterministic and only change with the `.defs` files. Once a macOS
CI run of the kernel succeeds, upload `build/xnu/obj` (the `installhdrs` EXPORT_HDRS and the
generated `*User.c`/`*Server.c`) as an artifact: a Linux build with those files in place
skips mig entirely (`make` sees the outputs), and everything after that (archives, link,
prelink) already runs with the LLVM tools. Kernel-source iteration then does not need macOS.

The tool shims of the attempt live in `~/Xnu4Sirius/tools-shim` (xcrun, ditto, ld) and
`~/Xnu4Sirius/mig-compat` (host header compat) on the build host; see the commit history of
this document for the exact errors encountered.
