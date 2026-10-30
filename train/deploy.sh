#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
# Export a trained run to ONNX at the detector size and compile it to an
# INT8 cvimodel for cv181x, calibrated on work/calib/personW (make_calib.py).
# EVAL_SIM=1 also measures COCO person AP in the simulator (~1.5 h); the
# board is faster: train/eval_all.sh.
#
#   train/deploy.sh RUN [W H]     e.g. train/deploy.sh yolo11n-silu 480 256
#
# Output: work/deploy/RUN-WxH.{onnx,outputs,cvimodel}
set -e
REPO=$(cd "$(dirname "$0")/.." && pwd)
W=${2:-480} H=${3:-256}
RUN=$1 N=$1-${W}x${H}
OUT=$REPO/work/deploy
VENV=$REPO/work/.venv-ultra/bin/python
TPU="docker run --rm -u $(id -u):$(id -g) -e HOME=/tmp -v $REPO:/repo tpu-mlir:1.30.2"
P=/usr/local/lib/python3.10/dist-packages/tpu_mlir

mkdir -p "$OUT"
cp "$REPO/work/train/runs/$RUN/weights/best.pt" "$OUT/$N.pt"
(cd "$OUT" && $VENV -c "from ultralytics import YOLO
YOLO('$N.pt').export(format='onnx', imgsz=($H, $W), opset=13, simplify=True)" >/dev/null)
$VENV "$REPO/train/heads.py" "$OUT/$N.onnx"

$TPU sh -c "set -e; mkdir -p /repo/work/deploy/$N.build; cd /repo/work/deploy/$N.build
model_transform.py --model_name $N --model_def ../$N.onnx \
	--input_shapes [[1,3,$H,$W]] --mean 0,0,0 \
	--scale 0.0039216,0.0039216,0.0039216 --pixel_format rgb \
	--output_names \"\$(cat ../$N.outputs)\" --mlir $N.mlir
run_calibration.py $N.mlir --dataset /repo/work/calib/person$W --input_num 400 \
	-o cali_table
model_deploy.py --mlir $N.mlir --quantize INT8 --calibration_table cali_table \
	--processor cv181x --quant_input --quant_output --model ../$N.cvimodel" \
	> "$OUT/$N.compile.log" 2>&1

[ "$EVAL_SIM" = 1 ] || exit 0
$TPU env PYTHONPATH=$P/python LD_LIBRARY_PATH=$P/lib python3 /repo/tools/eval_person.py \
	--onnx /repo/work/deploy/$N.onnx --heads /repo/work/deploy/$N.outputs \
	--cvimodel /repo/work/deploy/$N.cvimodel --size ${W}x${H} \
	--images /repo/work/datasets/coco-person/images/val2017 \
	--ann /repo/work/datasets/dl/annotations/instances_val2017.json \
	--out /repo/work/deploy/$N.ap.json > "$OUT/$N.eval.log" 2>&1
cat "$OUT/$N.ap.json"
