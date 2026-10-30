#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
# Cross-build in a container: build.sh [arm64|riscv64] [make args]
#
# CVIRT is the TPU runtime install directory for the target
# (default build/cviruntime/<arch>/install, from tools/build-cviruntime.sh).
# The container image (docker/Dockerfile) is made on first use.
set -e
cd "$(dirname "$0")"
ARCH=${1:-arm64}
case $ARCH in
arm64)   CROSS=aarch64-linux-gnu- ;;
riscv64) CROSS=riscv64-linux-gnu- ;;
*)       echo "usage: $0 [arm64|riscv64] [make args]" >&2; exit 1 ;;
esac
[ $# -gt 0 ] && shift
CVIRT=$(cd "${CVIRT:-build/cviruntime/$ARCH/install}" 2>/dev/null && pwd) || {
	echo "no TPU runtime: run tools/build-cviruntime.sh $ARCH SDK, or set CVIRT" >&2
	exit 1
}
IMG=duos-detect-build:$ARCH
docker image inspect "$IMG" >/dev/null 2>&1 ||
	docker build -t "$IMG" --build-arg ARCH="$ARCH" docker
docker run --rm -u "$(id -u):$(id -g)" -v "$PWD:/src" -v "$CVIRT:/rt:ro" -w /src \
	"$IMG" make CROSS_COMPILE=$CROSS CVIRT=/rt "$@"
