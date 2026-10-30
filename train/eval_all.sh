#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
# Person AP of each model on COCO, VOC2007, CrowdHuman and MOT17: float on
# this PC, INT8 on the board with the installed duos-detect. Copy each
# NAME.cvimodel to the board's $REMOTE directory first. BOARD_CMD is the
# command that runs a shell command on the board, e.g. "ssh root@duos".
#
#   train/eval_all.sh MODEL...    (MODEL: path without extension, *.onnx,
#                                  *.outputs; $REMOTE/NAME.cvimodel on board;
#                                  NAME ends in -WxH, the detector size)
# SETS limits the test sets, e.g. SETS="mot17 crowdhuman".
REPO=$(cd "$(dirname "$0")/.." && pwd)
PY=$REPO/work/.venv-ultra/bin/python
D=$REPO/work/datasets
BOARD_CMD=${BOARD_CMD:?set BOARD_CMD, e.g. "ssh root@duos"}
REMOTE=${REMOTE:-/root/models}
OUT=$REPO/work/eval
mkdir -p "$OUT"

for m in "$@"; do
	name=$(basename "$m")
	size=${name##*-}
	for set in "coco $D/coco-person/images/val2017 $D/dl/annotations/instances_val2017.json 100" \
		   "voc $D/voc/VOCdevkit $D/voc/person-test.json 100" \
		   "crowdhuman $D/crowdhuman/Images $D/crowdhuman/person-val-fbox.json 300" \
		   "crowdhuman-vbox $D/crowdhuman/Images $D/crowdhuman/person-val-vbox.json 300" \
		   "mot17 $D/mot17/train $D/mot17/person-train.json 300"; do
		set -- $set
		case " ${SETS:-$1} " in *" $1 "*) ;; *) continue ;; esac
		$PY "$REPO/tools/eval_person.py" --onnx "$m.onnx" --heads "$m.outputs" \
			--images "$2" --ann "$3" --max-dets "$4" --size "$size" \
			--board "$BOARD_CMD 'duos-detect -m $REMOTE/$name.cvimodel -i - -c 0.001 -I 0.7 -M $4'" \
			--out "$OUT/$name.$1.json" --save-dets "$OUT/$name.$1.dets" \
			> "$OUT/$name.$1.log" 2>&1
		echo "$name $1 exit $?"
	done
done
