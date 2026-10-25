#!/usr/bin/env bash
#
# xnu-container.sh -- run the XNU Linux kernel build inside the toolchain image.
#
#   ./scripts/xnu-container.sh build-image   build (or rebuild) localhost/unixnu-xnu-linux:fedora44
#   ./scripts/xnu-container.sh check         the installer's host-dependency check inside the image
#   ./scripts/xnu-container.sh shell         an interactive shell with the workspace mounted
#   ./scripts/xnu-container.sh run CMD...    run one command in that shell's environment
#
# The toolchain (clang, cctools ld64, mig, iig, lzfse, shims) lives in the image. Nothing is
# installed on the host: no /opt/unixnu-linux, no /usr/bin shims, no distribution packages.
#
# The workspace is mounted at its own absolute path, so the absolute paths recorded inside
# out/ (compile databases, .d files, stamps) stay valid between host and container, and an
# incremental build continues where the previous one stopped.
#
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)                     # the device repository (this one)
ROOT=$(cd "$REPO/.." && pwd)                     # the workspace holding UniXNU and the device repo
UNIXNU=${UNIXNU_DIR:-$ROOT/UniXNU}
IMAGE=${XNU_IMAGE:-localhost/unixnu-xnu-linux:fedora44}
PROXY=${HTTPS_PROXY:-${https_proxy:-http://127.0.0.1:7897}}

# prepare-xnu-source.py restores the DriverKit headers Apple omits from the newer source through
# the GitHub contents API, whose anonymous quota is per address and shared by everything behind a
# proxy (HTTP 403 "rate limit exceeded" on the first cold build). Forward a token: an exported
# GITHUB_TOKEN/GH_TOKEN, or the one the GitHub CLI already holds. It is passed to the container
# environment only; it is never written to the workspace or to the image.
TOKEN=${GITHUB_TOKEN:-${GH_TOKEN:-}}
if [ -z "$TOKEN" ] && command -v gh >/dev/null 2>&1; then
	TOKEN=$(gh auth token 2>/dev/null || true)
fi

# keep-id: files written into the mounted workspace stay owned by the host user.
# host network: the build reaches GitHub and crates.io through the host proxy on 127.0.0.1.
# :z relabels the mount for SELinux (Fedora is enforcing by default).
FLAGS=(--userns=keep-id --network=host
       -e HOME=/tmp -e USER="${USER:-builder}"
       -e HTTP_PROXY="$PROXY" -e HTTPS_PROXY="$PROXY"
       -e http_proxy="$PROXY" -e https_proxy="$PROXY"
       -v "$ROOT:$ROOT:z"
       -w "$UNIXNU")
[ -n "$TOKEN" ] && FLAGS+=(-e "GITHUB_TOKEN=$TOKEN" -e "GH_TOKEN=$TOKEN")

usage() { sed -n '3,18p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; }

case "${1:-}" in
build-image)
	[ -d "$UNIXNU/scripts/linux" ] || { echo "no UniXNU checkout at $UNIXNU (set UNIXNU_DIR)" >&2; exit 1; }
	podman build --network=host \
		--build-arg "https_proxy=$PROXY" --build-arg "http_proxy=$PROXY" \
		-t "$IMAGE" \
		-f "$REPO/containers/xnu-linux/Containerfile" \
		"$UNIXNU/scripts/linux"
	;;
check)
	podman run --rm "${FLAGS[@]}" "$IMAGE" python3 scripts/linux/setup-toolchain.py --check
	;;
shell)
	podman run --rm -it "${FLAGS[@]}" "$IMAGE" bash
	;;
run)
	shift
	[ $# -gt 0 ] || { usage; exit 2; }
	podman run --rm -it "${FLAGS[@]}" "$IMAGE" "$@"
	;;
*)
	usage
	exit 2
	;;
esac
