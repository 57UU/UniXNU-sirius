# Building the XNU kernel on Linux (container)

The kernel step of UniXNU targets macOS + Xcode. On Linux it instead needs a Mach-O toolchain
that Xcode provides on a Mac. UniXNU ships that toolchain as an installer
(`scripts/linux/setup-toolchain.py`), but the installer is invasive: it builds a patched LLVM
from source, writes `/opt/unixnu-linux`, installs `xcrun`/`plutil`/`codesign` into `/usr/bin`,
and installs a dozen distribution packages.

This directory puts all of that inside a Podman image. The host is left untouched.

## What is in the image

| Path | Contents |
| --- | --- |
| `/opt/unixnu-linux/llvm/bin/clang` | LLVM at UniXNU's pinned commit with `llvm-xnu-builtin-type.patch` (`__builtin_xnu_type_*`, which xnu's `kalloc.h` needs), AArch64 target only |
| `/opt/unixnu-linux/CommandLineTools/usr/bin` | `ld`/`ar`/`strip`/`nm`/`lipo`/`libtool` from cctools-port `1030.6.3-ld64-956.6`, `xnu-host-cc`, `gm4`, `install` |
| `/opt/unixnu-linux/CommandLineTools/usr/libexec/migcom` | MIG from `bootstrap_cmds-138` |
| `/opt/unixnu-linux/XcodeDefault.xctoolchain/usr/bin/iig` | DriverKit interface generator (`iig-port`) |
| `/opt/unixnu-linux/bin/lzfse` | Kernelcache decode |
| `/usr/bin/xcrun`, `/usr/bin/plutil`, `/usr/bin/codesign` | Shims; xnu's makefiles look for these by absolute path |

`ld64.lld` from LLVM cannot replace cctools `ld64` for a kernel link: it lacks
`-image_base`, `-segaddr`, `-segment_order`, `-alias_list`, `-version_load_command` and
`-ios_version_min`.

## Build the image

```sh
cd sirius
./scripts/xnu-container.sh build-image
```

It builds from `UniXNU/scripts/linux` (the installer needs nothing else) against Fedora 44,
which matches a Fedora host and has the `dnf` path the installer supports. Expect 20–45
minutes (LLVM dominates) and a 4–6 GB image.

Rebuild only when `UniXNU/scripts/linux` changes (the installer, its LLVM patch, the shims):

```sh
./scripts/xnu-container.sh build-image
```

### Proxy

The image is built with `--network=host` and passes `https_proxy`/`http_proxy` as build
arguments, so `dnf` and the source clones go through the host proxy. The default is
`http://127.0.0.1:7897`; override with `HTTPS_PROXY=... ./scripts/xnu-container.sh build-image`.
The proxy is a build argument, never baked into a layer.

## Use it

```sh
./scripts/xnu-container.sh check          # the installer's own dependency check
./scripts/xnu-container.sh shell          # interactive, workspace mounted
./scripts/xnu-container.sh run bash -lc './unixnu build kernel -D ../sirius'
```

The workspace is mounted at its **own absolute path** with `--userns=keep-id`:

- files written into `out/` stay owned by the host user;
- absolute paths recorded in compile databases, `.d` files and tree stamps stay valid, so a
  host-side inspection and an in-container rebuild agree, and incremental builds survive.

The runtime flags are `--network=host` (GitHub through the host proxy), `:z` on the mount
(SELinux is enforcing on Fedora), and `HOME=/tmp` (the image has no user entry for the host
uid).

### GitHub API quota

`prepare-xnu-source.py` restores the DriverKit headers Apple dropped from the newer XNU source
through the GitHub *contents API*. Anonymous calls share a small per-address quota, and behind a
proxy that address is shared by everything else using it: the first cold build stops with
`HTTP Error 403: rate limit exceeded`.

The wrapper therefore forwards a token into the container environment (never into the image or
the workspace): an exported `GITHUB_TOKEN`/`GH_TOKEN`, or `gh auth token` when the GitHub CLI is
logged in. With a token the quota is 5000/hour. After the first successful run the header
manifest is cached in the tree's `.cache/sources/`, so later builds do not need it.

### Prelink

`build kernel` also prelinks when the original kernelcache is reachable: `unixnu ipsw fetch`
downloads only the members `[ipsw] fetch` names (about 45 MB here, not the whole 7 GB IPSW) into
`out/ipsw/21H461/`.

## Cleanup

```sh
podman rmi localhost/unixnu-xnu-linux:fedora44
podman system prune
```

Nothing else is left behind: no `/opt/unixnu-linux`, no `/usr/bin/xcrun`, no package changes
on the host.

## Limits

- The image is tied to a UniXNU revision. A `scripts/linux` update means a rebuild.
- The kernel *build* runs in the container; the *inputs* are the host's. Prepending an IPSW or
  a kernelcache does not need the container, `unixnu ipsw fetch` and `build prelink` run on the
  host as well.
- The fallback, if the patched LLVM cannot be built inside the image, is
  [Allenwdk/xnu-linux-toolchain](https://github.com/Allenwdk/xnu-linux-toolchain): the system
  clang plus `portable-kalloc-type-builtins.patch`, cctools-port and a Python `iig`. It removes
  the LLVM build but needs a layout adapter for the paths the driver checks. See
  `docs/linux-kernel-build.md`.
