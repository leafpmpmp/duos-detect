#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
# Cross-build cpufe for the Cortex-A53.
set -e
cd "$(dirname "$0")"
docker run --rm -u "$(id -u):$(id -g)" -v "$PWD:/src" -w /src tpu-xbuild-arm64 \
	aarch64-linux-gnu-gcc -O3 -mcpu=cortex-a53 -Wall -Wextra -o cpufe cpufe.c -lm
