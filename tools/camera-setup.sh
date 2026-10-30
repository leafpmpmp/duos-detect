#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
# Set up the J2 camera pipeline for /dev/video0 (run on the board, as root).
#
#   camera-setup.sh [width height] [pattern]
#
# Pattern 0 (default) is the live image, 1 the sensor's colour bars.
# Prints the V4L2 pixel format that /dev/video0 delivers.
M="media-ctl -d /dev/media0"
W=${1:-2304} H=${2:-1296} TP=${3:-0}

SENSOR=$($M -p | sed -n 's/^.*<- "\([^"]*\)":0 \[ENABLED.*$/\1/p' | grep -v '^cv1800' | head -1)
[ -n "$SENSOR" ] || { echo "no sensor in the media graph" >&2; exit 1; }
# The module is mounted upside down: flip both axes for an upright image
v4l2-ctl -d "$($M -e "$SENSOR")" -c horizontal_flip=1,vertical_flip=1,test_pattern="$TP" || exit 1
CODE=$($M --get-v4l2 "\"$SENSOR\":0" | sed -n 's/.*fmt:\([A-Z0-9_]*\)\/.*/\1/p')
for e in "$SENSOR" cv1800-csi2 cv1800-vi-bridge; do
	$M -V "\"$e\":0 [fmt:$CODE/${W}x${H}]" || exit 1
done
case $CODE in
	SBGGR*) PIX=pBCC ;; SGBRG*) PIX=pGCC ;; SGRBG*) PIX=pgCC ;; *) PIX=pRCC ;;
esac
v4l2-ctl -d /dev/video0 --set-fmt-video width="$W",height="$H",pixelformat=$PIX >&2 || exit 1
echo "$SENSOR: $CODE ${W}x${H} -> $PIX, pattern $TP" >&2
echo $PIX
