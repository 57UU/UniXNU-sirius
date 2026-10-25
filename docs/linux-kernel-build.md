# Building the kernel on Linux

The kernel step is designed for macOS + Xcode (see `.github/workflows/kernel.yml`). On Linux
UniXNU supplies the same toolchain itself: `scripts/linux/setup-toolchain.py` builds a patched
LLVM, cctools-port's `ld64`, MIG, `iig`, `lzfse` and the `xcrun`/`plutil`/`codesign` shims, and
`unixnu build kernel` detects the host and sets the environment (`linux_env()` in
`scripts/unixnu/kernel.py`, `scripts/linux/prepare-xnu.py` in the build tree).

**This port runs that installer inside a container**, so nothing is installed on the host. See
[`containers/xnu-linux/`](../containers/xnu-linux/) for the image and
`scripts/xnu-container.sh` for the wrapper.

```sh
cd sirius
./scripts/xnu-container.sh build-image            # once, 20–45 min, 4–6 GB image
./scripts/xnu-container.sh check                  # host dependencies of the installer
./scripts/xnu-container.sh run bash -lc '
  ./unixnu defconfig sirius_defconfig -D ../sirius &&
  ./unixnu fetch &&
  ./unixnu build kernel -D ../sirius'
```

Outputs land in `out/sirius/ipados-17.7.11-21H461/sirius/kernel/` on the host: `kernel.macho`
(50 MB prelinked, `__TEXT` at `0xfffffff007004000`, 30 kernelcache components and 69294
bindings) and `kernel.json`. The workspace is mounted at its own absolute path, so incremental
builds and the recorded compile commands stay valid.

Two things the first successful run needed:

- **A GitHub token.** `prepare-xnu-source.py` fetches the DriverKit headers Apple omitted from
  the newer source through the GitHub contents API. Anonymous quota is per address, and a proxy
  shares it, so the first build stops with HTTP 403. `scripts/xnu-container.sh` forwards
  `GITHUB_TOKEN`/`GH_TOKEN`, or `gh auth token` when the CLI is logged in.
- **The prelink inputs.** `build kernel` prelinks in the same run when the kernelcache is
  available; `unixnu ipsw fetch` pulls only the members `[ipsw] fetch` names, about 45 MB.

## What was established before the container

The first attempt (2026-10-24) drove the toolchain by hand and reached a wrong conclusion that
is worth recording, because it is the reason the container image exists:

| Tool | Then | Now |
| --- | --- | --- |
| `xcrun` | XNU's makefiles call `/usr/bin/xcrun` by absolute path; a PATH shim only covered `-find`/`--show-sdk-path` | The installer places the shim where the makefiles look |
| `sysctl -n hw.memsize` | hardcoded `/usr/sbin/sysctl` in `makedefs/MakeInc.top` | shim, same place |
| `ditto` | macOS-only; `cp -a` wrapper worked | replaced by the installer's `cp -R` handling in the patched script |
| `mig`/`migcom` | **believed unportable**: the host headers hit glibc/BSD typedef conflicts (`__int64_t` long vs long long) | built from `bootstrap_cmds-138` with the Darwin-isms in a `-idirafter` compatibility directory |
| Linking | `ld64.lld` plus a `-Wl,-ios_version_min` → `-Wl,-platform_version,ios,17.0,17.0` translation | cctools `ld64`; `ld64.lld` cannot link a kernel (`-image_base`, `-segaddr`, `-segment_order`, `-alias_list`, `-version_load_command`, `-ios_version_min`) |

The lesson: the missing piece was never a shim, it was that MIG and `ld64` had to be *built* for
Linux. Both are now part of the toolchain, which is what makes the container possible.

## Prelink on Linux

`build kernel` stops at the native Mach-O. `build prelink` needs the original 21H461 kernelcache
(`unixnu ipsw fetch` downloads the IPSW, about 7 GB; or pass `--kernelcache FILE`). That step
runs on the host too, `lzfse` is only needed for decoding and it is in the image.

## Fallback

If the patched LLVM cannot be built inside the image (a newer distribution GCC building an older
pinned LLVM), [Allenwdk/xnu-linux-toolchain](https://github.com/Allenwdk/xnu-linux-toolchain)
reaches the same result with the **system** clang:

- `bootstrap.sh` builds MIG (`bootstrap_cmds` `cross_platform`), cctools-port, `lzfse`,
  `unifdef`, `tcsh` and installs the shims;
- `portable-kalloc-type-builtins.patch` replaces the Apple-only `__builtin_xnu_type_*` with a
  data-only summary, so xnu compiles with upstream clang. Its five patches are byte-identical to
  `patches/xnu-linux/` of this repository.

Adopting it needs a layout adapter for the seven paths `linux_env()` checks, and
`portable-kalloc-type-builtins.patch` added to the series (it is `__has_builtin`-guarded, so it
is inert on Apple clang and safe for the macOS CI). The other four patches must **not** be
added: `prepare-xnu.py` already performs those source rewrites, and both together conflict.

The third repository that comes up in this context,
[ShueiTsuki/xnu-ios-build-linux](https://github.com/ShueiTsuki/xnu-ios-build-linux), does not
build a kernel. It builds iOS userland and GPU pieces (the NativeMetal plugin, Turnip, `vktest`,
`mtltest`) with clang 18, `ld64.lld`, an iPhoneOS SDK and `rcodesign`. It becomes relevant when
this port needs iOS-side binaries, not for the kernel.
