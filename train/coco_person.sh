#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
# Make the person-only COCO 2017 set in Ultralytics YOLO format: every
# image with a person plus 10% of the other train images as negatives, and
# all val images. The labels keep class 0 (person) only.
#
#   train/coco_person.sh [DIR]     (default work/datasets/coco-person)
#
# Fetches the Ultralytics COCO labels, then the images (12 GB). Run it
# again to retry failed downloads. Then train with DIR/coco-person.yaml.
# DIR/instances_val2017.json is the COCO ground truth for the evaluation.
set -e
REPO=$(cd "$(dirname "$0")/.." && pwd)
DIR=${1:-$REPO/work/datasets/coco-person}
mkdir -p "$DIR"
DIR=$(cd "$DIR" && pwd)
ZIP=$DIR/coco2017labels.zip
[ -f "$ZIP" ] || curl -fL -o "$ZIP" \
	https://github.com/ultralytics/assets/releases/download/v0.0.0/coco2017labels.zip

[ -f "$DIR/train2017.urls" ] || python3 -I - "$ZIP" "$DIR" <<'EOF'
import os
import random
import sys
import zipfile

z, dst = zipfile.ZipFile(sys.argv[1]), sys.argv[2]
names = set(z.namelist())
random.seed(0)
for split in ("train2017", "val2017"):
    os.makedirs(f"{dst}/labels/{split}", exist_ok=True)
    pos, neg = [], []
    for line in z.read(f"coco/{split}.txt").decode().split():
        im = os.path.basename(line)
        lf = f"coco/labels/{split}/{im[:-4]}.txt"
        rows = z.read(lf).decode().splitlines(True) if lf in names else []
        persons = [r for r in rows if r.split()[0] == "0"]
        if persons:
            pos.append(im)
            with open(f"{dst}/labels/{split}/{im[:-4]}.txt", "w") as f:
                f.writelines(persons)
        else:
            neg.append(im)
    # val: every image, as in COCO val2017; train: 10% negatives
    keep = neg if split == "val2017" else random.sample(neg, len(pos) // 10)
    for im in keep:
        open(f"{dst}/labels/{split}/{im[:-4]}.txt", "w").close()
    sel = sorted(pos + keep)
    with open(f"{dst}/{split}.txt", "w") as f:
        f.writelines(f"./images/{split}/{im}\n" for im in sel)
    with open(f"{dst}/{split}.urls", "w") as f:
        f.writelines(f"http://images.cocodataset.org/{split}/{im}\n" for im in sel)
    print(f"{split}: {len(pos)} images with a person, {len(keep)} without")
with open(f"{dst}/instances_val2017.json", "wb") as f:
    f.write(z.read("coco/annotations/instances_val2017.json"))
with open(f"{dst}/coco-person.yaml", "w") as f:
    f.write(f"# COCO 2017, person class only\npath: {dst}\n"
            "train: train2017.txt\nval: val2017.txt\nnames:\n  0: person\n")
EOF

cd "$DIR"
for split in val2017 train2017; do
	mkdir -p images/$split
	ls images/$split > have.tmp
	grep -v -F -f have.tmp $split.urls > need.tmp || true
	echo "$split: $(wc -l < need.tmp) images to fetch"
	(cd images/$split && xargs -P 32 -n 16 curl -s -f --retry 5 --retry-delay 2 \
		--remote-name-all < ../../need.tmp) || true
	echo "$split: $(ls images/$split | wc -l) of $(wc -l < $split.urls) present"
done
rm -f have.tmp need.tmp
