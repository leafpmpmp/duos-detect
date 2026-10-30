# SPDX-License-Identifier: AGPL-3.0-or-later
"""Build an INT8 calibration set at the detector input size: letterboxed
COCO person images plus front-end camera frames.

    make_calib.py OUTDIR --coco-list train2017.txt --coco-root DIR
                  [--camera DIR] [--n 300] [--size 480x256]
"""
import argparse
import os
import random

import cv2
import numpy as np

ap = argparse.ArgumentParser()
ap.add_argument("out")
ap.add_argument("--coco-list", required=True)
ap.add_argument("--coco-root", required=True)
ap.add_argument("--camera")
ap.add_argument("--n", type=int, default=300)
ap.add_argument("--size", default="480x256")
a = ap.parse_args()
w, h = map(int, a.size.split("x"))
os.makedirs(a.out, exist_ok=True)

random.seed(0)
names = [l.strip() for l in open(a.coco_list)]
for rel in random.sample(names, a.n):
    img = cv2.imread(os.path.join(a.coco_root, rel))
    if img is None:
        continue
    s = min(w / img.shape[1], h / img.shape[0])
    nw, nh = round(img.shape[1] * s), round(img.shape[0] * s)
    out = np.full((h, w, 3), 114, np.uint8)
    x, y = (w - nw) // 2, (h - nh) // 2
    out[y:y + nh, x:x + nw] = cv2.resize(img, (nw, nh), interpolation=cv2.INTER_LINEAR)
    cv2.imwrite(os.path.join(a.out, "coco_" + os.path.basename(rel)[:-4] + ".png"), out)
if a.camera:
    for f in sorted(os.listdir(a.camera)):
        img = cv2.imread(os.path.join(a.camera, f))
        cv2.imwrite(os.path.join(a.out, "cam_" + f), cv2.resize(img, (w, h)))
print(len(os.listdir(a.out)), "calibration images in", a.out)
