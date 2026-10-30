#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
# Train the SiLU and then the ReLU person detector (low CPU priority; one at
# a time: two runs do not fit in 8 GB of GPU memory), and export, compile
# and evaluate each at 480x256.
REPO=$(cd "$(dirname "$0")/.." && pwd)
PY=$REPO/work/.venv-ultra/bin/python
DATA=$REPO/work/datasets/coco-person/coco-person.yaml
LOG=$REPO/work/train
mkdir -p "$LOG"

for run in "silu 60" "relu 100"; do
	set -- $run
	nice -n 10 $PY "$REPO/train/train.py" yolo11n-$1 --act $1 --epochs $2 \
		--data "$DATA" > "$LOG/yolo11n-$1.log" 2>&1
	echo "$1 training exit $?"
	"$REPO/train/deploy.sh" yolo11n-$1 && echo "$1 deploy ok"
done
