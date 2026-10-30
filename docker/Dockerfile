# SPDX-License-Identifier: AGPL-3.0-or-later
# Cross-build image for one target. build.sh makes it on first use:
#   docker build -t duos-detect-build:riscv64 --build-arg ARCH=riscv64 docker
FROM debian:trixie-slim
ARG ARCH=riscv64
RUN case "$ARCH" in \
	arm64) T=aarch64-linux-gnu ;; \
	riscv64) T=riscv64-linux-gnu ;; \
	*) echo "ARCH must be arm64 or riscv64" >&2; exit 1 ;; \
	esac && \
	dpkg --add-architecture "$ARCH" && apt-get update && \
	apt-get install -y --no-install-recommends gcc-$T g++-$T \
		libjpeg62-turbo-dev:"$ARCH" cmake ninja-build make gcc g++ git \
		ca-certificates python3 && \
	rm -rf /var/lib/apt/lists/*
