#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
# Capture live frames as packed RAW12 files (run on the board, as root).
#
#   capture.sh outdir [count] [interval-seconds]
#
# Takes one frame every interval, each from a new short stream, so frames
# differ when the scene moves.
OUT=${1:?outdir} N=${2:-16} DT=${3:-1}
HERE=$(dirname "$0")
sh "$HERE/camera-setup.sh" 2304 1296 0 >/dev/null || exit 1
mkdir -p "$OUT"
i=0
while [ $i -lt "$N" ]; do
	f=$(printf "%s/frame%03d.raw" "$OUT" $i)
	# Skip early frames: the sensor needs a few to settle
	v4l2-ctl -d /dev/video0 --stream-mmap=4 --stream-skip=4 --stream-count=1 \
		--stream-to="$f" >/dev/null 2>&1 || exit 1
	i=$((i + 1))
	sleep "$DT"
done
ls "$OUT" | wc -l
