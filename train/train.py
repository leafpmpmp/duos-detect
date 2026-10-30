# SPDX-License-Identifier: AGPL-3.0-or-later
"""Fine-tune a person-only detector from COCO-pretrained Ultralytics weights.

    train.py NAME --base yolo11n --act silu|relu --epochs N [--imgsz 480]

The model keeps the base architecture; --act relu swaps every SiLU for ReLU
(faster on the cv181x TPU) and still starts from the pretrained conv
weights. Runs land in work/train/runs/NAME.
"""
import argparse
import os

import yaml
from ultralytics import YOLO
from ultralytics.nn.tasks import yaml_model_load

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "work", "train")

ap = argparse.ArgumentParser()
ap.add_argument("name")
ap.add_argument("--base", default="yolo11n")
ap.add_argument("--act", choices=["silu", "relu"], default="silu")
ap.add_argument("--data", required=True)
ap.add_argument("--epochs", type=int, default=60)
ap.add_argument("--imgsz", type=int, default=480)
ap.add_argument("--batch", type=int, default=64)
ap.add_argument("--workers", type=int, default=12)
a = ap.parse_args()
a.data = os.path.abspath(a.data)	# before the chdir below

cfg = yaml_model_load(a.base + ".yaml")
cfg["nc"] = 1
if a.act == "relu":
    cfg["activation"] = "nn.ReLU()"
cfg.pop("yaml_file", None)
os.makedirs(OUT, exist_ok=True)
os.chdir(OUT)	# pretrained weights download here
# The file name carries the model scale ("n") for Ultralytics
cfg_path = os.path.join(OUT, f"{a.base}-{a.act}-person.yaml")
with open(cfg_path, "w") as f:
    yaml.safe_dump(cfg, f, sort_keys=False)

model = YOLO(cfg_path).load(a.base + ".pt")
model.train(data=a.data, epochs=a.epochs, imgsz=a.imgsz, batch=a.batch,
            workers=a.workers, project=os.path.join(OUT, "runs"), name=a.name,
            exist_ok=True, plots=True, seed=0, deterministic=False)
