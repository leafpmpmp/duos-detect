#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
# Run zc_test on the board (as root) with the IMX708 on J2.
#
#   run.sh [pattern] [zc_test args...]
#
# Pattern 1 (default) is the sensor's colour bars, 0 is the live image.
# Needs zc_test, the model and the cviruntime libs in /root/tpu.
M="media-ctl -d /dev/media0"
W=2304 H=1296
TP=${1:-1}
[ $# -gt 0 ] && shift

SENSOR=$($M -p | sed -n 's/^.*<- "\([^"]*\)":0 \[ENABLED.*$/\1/p' | grep -v '^cv1800' | head -1)
[ -n "$SENSOR" ] || { echo "no sensor in the media graph" >&2; exit 1; }
SD=$($M -e "$SENSOR")
# The module is mounted upside down: flip both axes for an upright image
v4l2-ctl -d "$SD" -c horizontal_flip=1,vertical_flip=1,test_pattern="$TP"
CODE=$($M --get-v4l2 "\"$SENSOR\":0" | sed -n 's/.*fmt:\([A-Z0-9_]*\)\/.*/\1/p')
for e in "$SENSOR" cv1800-csi2 cv1800-vi-bridge; do
	$M -V "\"$e\":0 [fmt:$CODE/${W}x${H}]" || exit 1
done
case $CODE in
	SBGGR*) PIX=pBCC ;; SGBRG*) PIX=pGCC ;; SGRBG*) PIX=pgCC ;; *) PIX=pRCC ;;
esac
v4l2-ctl -d /dev/video0 --set-fmt-video width=$W,height=$H,pixelformat=$PIX || exit 1
echo "$SENSOR: $CODE ${W}x${H} -> $PIX, pattern $TP"

cd /root/tpu
LD_LIBRARY_PATH=/root/tpu/lib ./zc_test "$@" models/mobiledetv2-pedestrian-d0-ls-448.cvimodel
