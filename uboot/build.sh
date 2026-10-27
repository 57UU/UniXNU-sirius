#!/bin/sh
# build.sh: build this repository's U-Boot image (boot-unixnu.img) from the pinned
# uboot/src submodule and uboot/unixnu-sirius.env.
#
# The submodule is a shallow checkout of https://github.com/57UU/u-boot.git branch
# sirius/v2.0; it carries the board support and sirius-build.sh's ENV_FILE option.
# The UniXNU boot chain itself is in unixnu-sirius.env, not in the source.
#
# Needs an aarch64 cross compiler (CROSS_COMPILE) and mkbootimg on PATH; an aarch64
# host builds natively.
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
src="$here/src"
if [ ! -f "$src/sirius-build.sh" ]; then
	echo "uboot/src is empty: git submodule update --init --depth 1 uboot/src" >&2
	exit 1
fi

# Build into this repository instead of the distro layout the script defaults to.
OUT=${OUT:-"$here/build"}
IMG=${IMG:-"$here/boot-unixnu.img"}
mkdir -p "$OUT"
ENV_FILE="$here/unixnu-sirius.env" OUT="$OUT" IMG="$IMG" "$src/sirius-build.sh" "$@"
echo "built $IMG"
