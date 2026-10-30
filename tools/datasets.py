# SPDX-License-Identifier: AGPL-3.0-or-later
"""Convert person benchmarks to COCO-format ground truth for eval_person.py.

People become category 1. Regions that a benchmark marks as "ignore"
(crowds, reflections, difficult objects) become iscrowd boxes, so
detections inside them count neither as hits nor as false positives.

    datasets.py voc ROOT OUT.json           # VOCdevkit/VOC2007 test
    datasets.py crowdhuman ODGT IMAGES OUT.json [--box fbox|vbox]
    datasets.py mot17 ROOT OUT.json [--step N]   # MOT17Det/train
"""
import argparse
import json
import os
import xml.etree.ElementTree as ET

from PIL import Image


class Coco:
    def __init__(self):
        self.images, self.anns = [], []

    def image(self, file_name, w, h):
        i = len(self.images) + 1
        self.images.append({"id": i, "file_name": file_name, "width": w, "height": h})
        return i

    def box(self, img, x, y, w, h, ignore):
        if w <= 0 or h <= 0:
            return
        self.anns.append({"id": len(self.anns) + 1, "image_id": img, "category_id": 1,
                          "bbox": [x, y, w, h], "area": w * h, "iscrowd": int(bool(ignore))})

    def save(self, path):
        json.dump({"images": self.images, "annotations": self.anns,
                   "categories": [{"id": 1, "name": "person"}]}, open(path, "w"))
        n = sum(1 for a in self.anns if not a["iscrowd"])
        print(f"{path}: {len(self.images)} images, {n} people, "
              f"{len(self.anns) - n} ignore regions")


def voc(a):
    root = os.path.join(a.root, "VOC2007")
    c = Coco()
    for stem in open(os.path.join(root, "ImageSets/Main/test.txt")).read().split():
        t = ET.parse(os.path.join(root, "Annotations", stem + ".xml")).getroot()
        size = t.find("size")
        img = c.image(f"VOC2007/JPEGImages/{stem}.jpg", int(size.find("width").text),
                      int(size.find("height").text))
        for o in t.iter("object"):
            if o.find("name").text != "person":
                continue
            bb = o.find("bndbox")
            x1, y1, x2, y2 = (float(bb.find(k).text) for k in ("xmin", "ymin", "xmax", "ymax"))
            # VOC pixel indices are 1-based and inclusive
            c.box(img, x1 - 1, y1 - 1, x2 - x1 + 1, y2 - y1 + 1,
                  int(o.find("difficult").text))
    c.save(a.out)


def crowdhuman(a):
    c = Coco()
    for line in open(a.odgt):
        r = json.loads(line)
        name = r["ID"] + ".jpg"
        w, h = Image.open(os.path.join(a.images, name)).size
        img = c.image(name, w, h)
        for b in r["gtboxes"]:
            ignore = b["tag"] != "person" or b.get("extra", {}).get("ignore", 0)
            c.box(img, *b[a.box], ignore)
    c.save(a.out)


def mot17(a):
    """Positives: class 1 (pedestrian) with the consider flag set. Ignore:
    person on vehicle (2), static person (7), distractor (8), reflection
    (12), and pedestrians with the flag cleared."""
    c = Coco()
    for seq in sorted(os.listdir(a.root)):
        d = os.path.join(a.root, seq)
        if not os.path.isfile(os.path.join(d, "gt/gt.txt")):
            continue
        info = dict(l.strip().split("=", 1) for l in open(os.path.join(d, "seqinfo.ini"))
                    if "=" in l)
        w, h, n = int(info["imWidth"]), int(info["imHeight"]), int(info["seqLength"])
        ext = info.get("imExt", ".jpg")
        frames = {}
        for f in range(1, n + 1, a.step):
            frames[f] = c.image(f"{seq}/img1/{f:06d}{ext}", w, h)
        for l in open(os.path.join(d, "gt/gt.txt")):
            f, _, x, y, bw, bh, flag, cls = l.split(",")[:8]
            f, cls = int(f), int(cls)
            if f not in frames:
                continue
            if cls == 1:
                c.box(frames[f], float(x), float(y), float(bw), float(bh), int(flag) == 0)
            elif cls in (2, 7, 8, 12):
                c.box(frames[f], float(x), float(y), float(bw), float(bh), True)
    c.save(a.out)


ap = argparse.ArgumentParser()
sub = ap.add_subparsers(dest="cmd", required=True)
p = sub.add_parser("voc")
p.add_argument("root")
p.add_argument("out")
p = sub.add_parser("crowdhuman")
p.add_argument("odgt")
p.add_argument("images")
p.add_argument("out")
p.add_argument("--box", choices=["fbox", "vbox"], default="fbox")
p = sub.add_parser("mot17")
p.add_argument("root")
p.add_argument("out")
p.add_argument("--step", type=int, default=1, help="use every Nth frame")
a = ap.parse_args()
{"voc": voc, "crowdhuman": crowdhuman, "mot17": mot17}[a.cmd](a)
