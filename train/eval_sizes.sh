#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
# Float person AP of one model at several detector input sizes (GPU).
#   train/eval_sizes.sh DIR/NAME WxH...   (DIR/NAME-WxH.onnx and .outputs)
REPO=$(cd "$(dirname "$0")/.." && pwd)
PY=$REPO/work/.venv-ultra/bin/python
D=$REPO/work/datasets
OUT=$REPO/work/eval/sizes
mkdir -p "$OUT"
m=$1
shift
for wh in "$@"; do
	for set in "coco $D/coco-person/images/val2017 $D/dl/annotations/instances_val2017.json 100" \
		   "crowdhuman $D/crowdhuman/Images $D/crowdhuman/person-val-fbox.json 300" \
		   "crowdhuman-vbox $D/crowdhuman/Images $D/crowdhuman/person-val-vbox.json 300" \
		   "mot17 $D/mot17/train $D/mot17/person-train.json 300"; do
		set -- $set
		$PY "$REPO/tools/eval_person.py" --onnx "$m-$wh.onnx" --heads "$m-$wh.outputs" \
			--size $wh --images "$2" --ann "$3" --max-dets "$4" --gpu \
			--out "$OUT/$(basename $m)-$wh.$1.json" > "$OUT/$(basename $m)-$wh.$1.log" 2>&1
		echo "$wh $1 exit $?"
	done
done
