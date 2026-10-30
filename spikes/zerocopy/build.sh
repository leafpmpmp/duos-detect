#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
# Cross-build zc_test for arm64 against the ported cviruntime (CVIRT as in
# build.sh; run build.sh arm64 once first to make the container image).
set -e
cd "$(dirname "$0")"
RT=$(cd "${CVIRT:-../../build/cviruntime/arm64/install}" && pwd)
docker run --rm -u "$(id -u):$(id -g)" -v "$PWD:/src" -v "$RT:/rt:ro" -w /src \
	duos-detect-build:arm64 aarch64-linux-gnu-gcc -O2 -Wall -Wextra \
	-I/rt/include -o zc_test zc_test.c -L/rt/lib -lcviruntime -lcvikernel
