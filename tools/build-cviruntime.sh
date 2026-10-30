#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
# Build the TPU runtime (cviruntime, cvikernel) for one target:
#
#   tools/build-cviruntime.sh arm64|riscv64 SDK
#
# SDK is a checkout of branch mainline-tpu of
# https://github.com/leafpmpmp/duo-buildroot-sdk-v2, the vendor runtime
# ported to the mainline TPU driver. The build reads only its directories
# cviruntime, cvikernel, cvibuilder, flatbuffers and cnpy.
#
# Output: build/cviruntime/<arch>/install, the default CVIRT of build.sh.
set -e
REPO=$(cd "$(dirname "$0")/.." && pwd)
ARCH=$1
case $ARCH in
arm64)   T=aarch64-linux-gnu ;;
riscv64) T=riscv64-linux-gnu ;;
*)       echo "usage: $0 arm64|riscv64 SDK" >&2; exit 1 ;;
esac
SDK=$(cd "${2:?usage: $0 arm64|riscv64 SDK}" && pwd)
IMG=duos-detect-build:$ARCH
OUT=$REPO/build/cviruntime/$ARCH
mkdir -p "$OUT"
docker image inspect "$IMG" >/dev/null 2>&1 ||
	docker build -t "$IMG" --build-arg ARCH="$ARCH" "$REPO/docker"

docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -v "$SDK:/sdk:ro" -v "$OUT:/b" \
	"$IMG" sh -ec "
git config --global --add safe.directory '*'
I=/b/install
TC=/sdk/cviruntime/scripts/toolchain-$T.cmake
CM='cmake -G Ninja -DCMAKE_POLICY_VERSION_MINIMUM=3.5'
OPT='-DCMAKE_BUILD_TYPE=RELEASE -DCMAKE_C_FLAGS_RELEASE=-O3 -DCMAKE_CXX_FLAGS_RELEASE=-O3'

# Host tools: the flatbuffers compiler and the cvimodel schema
\$CM -S /sdk/flatbuffers -B /b/flatbuffers -DCMAKE_BUILD_TYPE=Release \
	-DFLATBUFFERS_BUILD_TESTS=OFF -DCMAKE_INSTALL_PREFIX=/b/flatbuffers-install
cmake --build /b/flatbuffers --target install >/dev/null
\$CM -S /sdk/cvibuilder -B /b/cvibuilder -DFLATBUFFERS_PATH=/b/flatbuffers-install \
	-DCMAKE_INSTALL_PREFIX=/b/cvibuilder-install
cmake --build /b/cvibuilder --target install >/dev/null

\$CM -S /sdk/cvikernel -B /b/cvikernel -DCHIP=cv181x -DCMAKE_TOOLCHAIN_FILE=\$TC \$OPT \
	-DCMAKE_INSTALL_PREFIX=\$I
cmake --build /b/cvikernel --target install >/dev/null

# zlib and cnpy: model_runner reads and writes .npz files
if [ ! -f \$I/lib/libz.a ]; then
	rm -rf /b/zlib
	git clone -q --depth 1 -b v1.3.1 https://github.com/madler/zlib /b/zlib
	cd /b/zlib
	CC=$T-gcc CFLAGS='-O2 -fPIC' ./configure --prefix=\$I --static >/dev/null
	make -j\$(nproc) >/dev/null
	make install >/dev/null
fi
\$CM -S /sdk/cnpy -B /b/cnpy -DCMAKE_TOOLCHAIN_FILE=\$TC -DCMAKE_BUILD_TYPE=RELEASE \
	-DCMAKE_FIND_ROOT_PATH=\$I -DZLIB_ROOT=\$I -DCMAKE_INSTALL_PREFIX=\$I
cmake --build /b/cnpy --target install >/dev/null

\$CM -S /sdk/cviruntime -B /b/cviruntime -DCHIP=cv181x -DRUNTIME=SOC \
	-DCMAKE_TOOLCHAIN_FILE=\$TC \$OPT -DCVIKERNEL_PATH=\$I -DCNPY_PATH=\$I \
	-DFLATBUFFERS_PATH=/b/flatbuffers-install -DCVIBUILDER_PATH=/b/cvibuilder-install \
	-DCMAKE_INSTALL_PREFIX=\$I -DENABLE_TEST=OFF
cmake --build /b/cviruntime --target install >/dev/null
"
echo "TPU runtime: $OUT/install"
