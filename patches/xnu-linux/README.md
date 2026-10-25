# Archived: Linux-host portability patches

These patches were applied by the legacy port's `scripts/build-xnu-4k-baseline.sh` **only when
building on a Linux host**. Format 2 moved that work into UniXNU itself
(`scripts/linux/prepare-xnu.py`, called by the shared `scripts/build-xnu-4k-baseline.sh`), so
nothing in this repository applies them any more.

They are kept as history and as a fallback when a Linux build fails on a host-tool rule that
UniXNU has not covered yet:

| Patch | Purpose |
| --- | --- |
| `iig-method-forwarders.patch` | host-tool source fixes for the Linux toolchain |
| `linux-host-tools.patch` | rewrites the SETUP host-tool rules (macOS assumptions) |
| `linux-link-shims.patch` | link-time shims for the Linux host |
| `llvm-clang-arm64-crypto-asm.patch` | upstream clang instead of Apple's for the crypto asm |
| `portable-kalloc-type-builtins.patch` | portable type builtins in kalloc |

To use one again, apply it from a board script on the Linux path and record the reason in
`porting.toml`; do not add these files to `patches/xnu/series`, whose entries must apply on the
macOS CI host as well.
