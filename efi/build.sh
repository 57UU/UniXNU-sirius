#!/usr/bin/env bash
# Build UniXNU with the EDK II build system.
#
#   ./build.sh [-b DEBUG|RELEASE] [-t CLANGPDB|GCC]
#
# UniXNU targets AArch64 UEFI only.
#
# The edk2 base defaults to a Mu_Basecore checkout in this directory (not tracked);
# UNIXNU_EDK2 may point at any other edk2 or Project Mu checkout that has MdePkg
# with BaseFdtLib.
# Results are copied to Build/Out/<ARCH>-<TARGET>/.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
EDK2=${UNIXNU_EDK2:-$ROOT/Mu_Basecore}
# Build products go to UNIXNU_BUILD_DIR when the unixnu driver sets it, otherwise next to the sources.
BUILD_DIR=${UNIXNU_BUILD_DIR:-$ROOT}
ARCH=AARCH64
TARGET=DEBUG
TOOLCHAIN=CLANGPDB
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}

usage() {
  sed -n '2,10p' "$0" | sed 's/^# \{0,1\}//'
  exit "${1:-0}"
}

while getopts "b:t:h" opt; do
  case $opt in
    b) TARGET=$OPTARG ;;
    t) TOOLCHAIN=$OPTARG ;;
    h) usage 0 ;;
    *) usage 1 ;;
  esac
done

if [ ! -f "$EDK2/MdePkg/MdePkg.dec" ]; then
  echo "error: no edk2 tree at $EDK2 (clone https://github.com/microsoft/mu_basecore, branch release/202608, into $ROOT/Mu_Basecore or set UNIXNU_EDK2)" >&2
  exit 1
fi

if [ ! -f "$EDK2/MdePkg/Library/BaseFdtLib/libfdt/libfdt/fdt.c" ] || [ ! -d "$EDK2/MdePkg/Library/MipiSysTLib/mipisyst/library/include" ]; then
  echo "error: MdePkg submodules are missing; run in $EDK2:" >&2
  echo "  git submodule update --init --depth 1 MdePkg/Library/BaseFdtLib/libfdt MdePkg/Library/MipiSysTLib/mipisyst BaseTools/Source/C/BrotliCompress/brotli" >&2
  exit 1
fi

# BaseTools host utilities (GenFw, GenFv, ...), built once from source.
if [ ! -x "$EDK2/BaseTools/Source/C/bin/GenFw" ]; then
  echo "== building BaseTools C utilities"
  make -C "$EDK2/BaseTools/Source/C" -j"$JOBS" >/dev/null
fi

export WORKSPACE=$BUILD_DIR
export PACKAGES_PATH=$ROOT:$EDK2
export EDK_TOOLS_PATH=$EDK2/BaseTools
export CONF_PATH=$BUILD_DIR/Build/Conf
export PYTHON_COMMAND=${PYTHON_COMMAND:-python3}
export PATH=$EDK_TOOLS_PATH/BinWrappers/PosixLike:$EDK_TOOLS_PATH/Source/C/bin:$PATH

case $TOOLCHAIN in
  CLANGPDB)
    if [ -z "${CLANG_BIN:-}" ]; then
      CLANG=$(command -v clang) || { echo "error: clang not found" >&2; exit 1; }
      CLANG_BIN=$(dirname "$CLANG")/
    fi
    export CLANG_BIN
    ;;
  GCC|GCC5)
    if [ "$(uname -m)" != aarch64 ]; then
      export GCC_AARCH64_PREFIX=${GCC_AARCH64_PREFIX:-aarch64-linux-gnu-}
      export GCC5_AARCH64_PREFIX=$GCC_AARCH64_PREFIX
    fi
    ;;
esac

mkdir -p "$CONF_PATH"
for f in target tools_def build_rule; do
  if [ ! -f "$CONF_PATH/$f.txt" ] || [ "$EDK_TOOLS_PATH/Conf/$f.template" -nt "$CONF_PATH/$f.txt" ]; then
    cp "$EDK_TOOLS_PATH/Conf/$f.template" "$CONF_PATH/$f.txt"
  fi
done

echo "== edk2: $EDK2"
echo "== build $ARCH $TARGET $TOOLCHAIN"
build -p UniXNUPkg/UniXNUPkg.dsc -a "$ARCH" -b "$TARGET" -t "$TOOLCHAIN" -n "$JOBS"

OUT=$BUILD_DIR/Build/Out/$ARCH-$TARGET
mkdir -p "$OUT"
BIN=$BUILD_DIR/Build/UniXNU/${TARGET}_$TOOLCHAIN/$ARCH
for m in UniXNU UniXNUDxe UniXNURamDiskDxe HelloPayload SampleDriver XnuTest; do
  cp "$BIN/$m.efi" "$OUT/"
done

echo "== outputs in $OUT"
ls -l "$OUT"
