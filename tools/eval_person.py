# SPDX-License-Identifier: AGPL-3.0-or-later
"""Person AP of a detector at the fixed deployment input, float and INT8.

Each image is letterboxed into the detector input (W x H). The float model
is the ONNX graph cut at the head convs (ONNX Runtime). The INT8 model runs
either on the board (--board: a shell command that runs
`duos-detect -i - -c 0.001 -I 0.7 -M 300` and so reads frames on stdin) or
in the TPU-MLIR simulator (--cvimodel, in the tpu-mlir container). Both
match bit for bit. Ground truth is COCO-format JSON (tools/datasets.py);
COCOeval scores category 1.

    eval_person.py --onnx M.onnx --heads M.outputs --images DIR --ann GT.json
                   [--board CMD | --cvimodel M.cvimodel] [--size 480x256]
                   [--cls 0] [--max-dets 100] [--limit N] [--out AP.json]
                   [--save-dets PREFIX]

Besides COCO AP it reports MR-2, the log-average miss rate over 0.01..1
false positives per image at IoU 0.5 (the CrowdHuman/CityPersons metric;
lower is better).
"""
import argparse
import json
import os
import queue
import subprocess
import sys
import tempfile
import threading
import time

import cv2
import numpy as np

PERSON = 1


def letterbox(img, w, h):
    s = min(w / img.shape[1], h / img.shape[0])
    nw, nh = round(img.shape[1] * s), round(img.shape[0] * s)
    out = np.full((h, w, 3), 114, np.uint8)
    x, y = (w - nw) // 2, (h - nh) // 2
    out[y:y + nh, x:x + nw] = cv2.resize(img, (nw, nh), interpolation=cv2.INTER_LINEAR)
    return out, s, x, y


def nms(boxes, scores, iou, max_det):
    area = (boxes[:, 2] - boxes[:, 0]) * (boxes[:, 3] - boxes[:, 1])
    order = scores.argsort()[::-1]
    keep = []
    while order.size and len(keep) < max_det:
        i, rest = order[0], order[1:]
        keep.append(i)
        w = np.clip(np.minimum(boxes[i, 2], boxes[rest, 2]) -
                    np.maximum(boxes[i, 0], boxes[rest, 0]), 0, None)
        h = np.clip(np.minimum(boxes[i, 3], boxes[rest, 3]) -
                    np.maximum(boxes[i, 1], boxes[rest, 1]), 0, None)
        inter = w * h
        order = rest[inter / (area[i] + area[rest] - inter + 1e-9) <= iou]
    return keep


def decode(outs, heads, cls, in_h, conf=0.001, iou=0.7, max_det=300):
    """outs: head name -> float [1, C, h, w]. Same math as src/detector.c."""
    levels = {}
    for name in heads:
        t = outs[name]
        levels.setdefault(t.shape[2:], {})["box" if "/cv2." in name else "cls"] = t[0]
    boxes, scores = [], []
    for (h, w), lv in levels.items():
        stride = in_h // h
        logit = lv["cls"][cls].ravel()
        idx = np.nonzero(logit >= np.log(conf / (1 - conf)))[0]
        if not idx.size:
            continue
        b = lv["box"].reshape(lv["box"].shape[0], -1)[:, idx]
        if b.shape[0] == 64:	# DFL: 4 sides x 16 bins
            b = b.reshape(4, 16, -1)
            e = np.exp(b - b.max(axis=1, keepdims=True))
            d = (e * np.arange(16)[None, :, None]).sum(1) / e.sum(1)
        else:
            d = b
        cx, cy = idx % w + 0.5, idx // w + 0.5
        boxes.append(np.stack([cx - d[0], cy - d[1], cx + d[2], cy + d[3]], 1) * stride)
        scores.append(1 / (1 + np.exp(-logit[idx])))
    if not boxes:
        return np.zeros((0, 4)), np.zeros(0)
    boxes, scores = np.concatenate(boxes), np.concatenate(scores)
    keep = nms(boxes, scores, iou, max_det)
    return boxes[keep], scores[keep]


def to_coco(img_id, boxes, scores, s, px, py):
    """Detector-input boxes -> COCO results in original image pixels."""
    boxes = (np.asarray(boxes, np.float64).reshape(-1, 4) - [px, py, px, py]) / s
    return [{"image_id": img_id, "category_id": PERSON,
             "bbox": [b[0], b[1], b[2] - b[0], b[3] - b[1]], "score": float(sc)}
            for b, sc in zip(boxes.tolist(), scores)]


class Board:
    """Streams frames to `duos-detect -i -` and collects its JSON lines."""

    def __init__(self, cmd):
        self.err = tempfile.TemporaryFile()
        self.p = subprocess.Popen(cmd, shell=True, stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=self.err)
        self.q = queue.Queue(maxsize=64)
        self.lines = []
        self.w = threading.Thread(target=self._write, daemon=True)
        self.r = threading.Thread(target=self._read, daemon=True)
        self.w.start()
        self.r.start()

    def _write(self):
        try:
            while (buf := self.q.get()) is not None:
                self.p.stdin.write(buf)
            self.p.stdin.close()
        except BrokenPipeError:
            pass	# put() reports it

    def _read(self):
        for line in self.p.stdout:
            self.lines.append(json.loads(line))

    def put(self, chw):
        while True:
            if not self.w.is_alive() or self.p.poll() is not None:
                self.err.seek(0)
                tail = self.err.read().decode(errors="replace")[-600:]
                sys.exit(f"board command ended early after {len(self.lines)} frames, "
                         f"exit {self.p.poll()}:\n{tail}")
            try:
                self.q.put(chw.tobytes(), timeout=1)
                return
            except queue.Full:
                pass

    def results(self):
        self.q.put(None)
        self.w.join()
        self.r.join()
        self.p.wait()
        return self.lines


def miss_rate(ev):
    """MR-2 at IoU 0.5 from COCOeval per-image matches (area "all")."""
    nimg = len(ev.params.imgIds)
    scores, tp, fp, ngt = [], [], [], 0
    for e in ev.evalImgs[:nimg]:	# first area range block: "all"
        if e is None:
            continue
        ngt += int((~np.asarray(e["gtIgnore"], bool)).sum())
        m = np.asarray(e["dtMatches"])[0] > 0
        ign = np.asarray(e["dtIgnore"])[0].astype(bool)
        scores += list(np.asarray(e["dtScores"])[~ign])
        tp += list(m[~ign])
        fp += list(~m[~ign])
    order = np.argsort(-np.asarray(scores), kind="stable")
    tp = np.cumsum(np.asarray(tp)[order])
    fp = np.cumsum(np.asarray(fp)[order])
    mr, fppi = 1 - tp / max(ngt, 1), fp / nimg
    out = []
    for ref in np.logspace(-2, 0, 9):
        i = np.nonzero(fppi <= ref)[0]
        out.append(mr[i[-1]] if i.size else 1.0)
    return float(np.exp(np.mean(np.log(np.maximum(out, 1e-10)))))


def coco_eval(ann, dets, img_ids, label, max_dets):
    from pycocotools.coco import COCO
    from pycocotools.cocoeval import COCOeval

    if not dets:
        print(f"{label}: no detections")
        return None
    gt = COCO(ann)
    ev = COCOeval(gt, gt.loadRes(dets), "bbox")
    ev.params.catIds = [PERSON]
    ev.params.imgIds = img_ids
    ev.params.maxDets = [1, 10, max_dets]
    ev.evaluate()
    ev.accumulate()
    print(f"== {label}")
    ev.summarize()
    # summarize() takes AP and AP75 at 100 detections; use max_dets
    p = ev.eval["precision"][:, :, 0, 0, 2]	# [IoU, recall], area all
    ap = lambda x: float(np.mean(x[x > -1])) if (x > -1).any() else -1.0
    st = dict(zip(["AP", "AP50", "AP75", "APs", "APm", "APl", "AR1", "AR10", "AR"],
                  (float(x) for x in ev.stats)))
    st.update(AP=ap(p), AP50=ap(p[0]), AP75=ap(p[5]), MR2=miss_rate(ev))
    print(f"AP {st['AP']:.3f} AP50 {st['AP50']:.3f} MR-2 {st['MR2']:.3f} "
          f"(at {max_dets} detections)")
    return st


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--onnx", required=True)
    ap.add_argument("--heads", required=True)
    ap.add_argument("--images", required=True)
    ap.add_argument("--ann", required=True)
    ap.add_argument("--board", help="shell command running duos-detect -i - on the board")
    ap.add_argument("--cvimodel", help="INT8 in the TPU-MLIR simulator instead")
    ap.add_argument("--size", default="480x256")
    ap.add_argument("--cls", type=int, default=0)
    ap.add_argument("--max-dets", type=int, default=100)
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--no-float", action="store_true")
    ap.add_argument("--gpu", action="store_true", help="float model on CUDA")
    ap.add_argument("--out", help="write the AP numbers here (JSON)")
    ap.add_argument("--save-dets", metavar="PREFIX",
                    help="write PREFIX.float.json and PREFIX.int8.json")
    a = ap.parse_args()
    in_w, in_h = map(int, a.size.split("x"))
    heads = open(a.heads).read().strip().split(",")

    sess = None
    if not a.no_float:
        import onnx
        import onnxruntime as ort

        sub = os.path.join(tempfile.mkdtemp(), "heads.onnx")
        onnx.utils.extract_model(a.onnx, sub, ["images"], heads)
        prov = ["CPUExecutionProvider"]
        if a.gpu:
            ort.preload_dlls()	# CUDA and cuDNN from the pip packages
            prov.insert(0, "CUDAExecutionProvider")
        sess = ort.InferenceSession(sub, providers=prov)
    board = Board(a.board) if a.board else None
    cvi = None
    if a.cvimodel:
        import pyruntime_cvi
        cvi = pyruntime_cvi.Model(a.cvimodel, output_all_tensors=False)
        cvi_heads = {o.name: next(h for h in heads if o.name.startswith(h))
                     for o in cvi.outputs}

    images = json.load(open(a.ann))["images"]
    if a.limit:
        images = images[:a.limit]
    dets = {"float": [], "int8": []}
    boxes_of = []	# letterbox parameters, in board order
    t0 = time.time()
    for n, im in enumerate(images):
        bgr = cv2.imread(os.path.join(a.images, im["file_name"]))
        img, s, px, py = letterbox(cv2.cvtColor(bgr, cv2.COLOR_BGR2RGB), in_w, in_h)
        chw = np.ascontiguousarray(img.transpose(2, 0, 1))
        if sess:
            outs = dict(zip(heads, sess.run(heads, {"images": chw[None].astype(np.float32) / 255})))
            dets["float"] += to_coco(im["id"], *decode(outs, heads, a.cls, in_h), s, px, py)
        if board:
            board.put(chw)
            boxes_of.append((im["id"], s, px, py))
        if cvi:
            cvi.inputs[0].data[:] = np.floor(chw[None].astype(np.float32) * 127 / 255 + 0.5).astype(np.int8)
            cvi.forward()
            outs = {cvi_heads[o.name]: o.data.astype(np.float32) * o.qscale for o in cvi.outputs}
            dets["int8"] += to_coco(im["id"], *decode(outs, heads, a.cls, in_h), s, px, py)
        if n % 500 == 499:
            print(f"{n + 1}/{len(images)} images, {(time.time() - t0) / (n + 1):.3f} s/image",
                  file=sys.stderr, flush=True)
    if board:
        lines = board.results()
        if len(lines) != len(boxes_of):
            sys.exit(f"board returned {len(lines)} results for {len(boxes_of)} frames")
        for r, (img_id, s, px, py) in zip(lines, boxes_of):
            b = np.array(r["boxes"]).reshape(-1, 5)
            dets["int8"] += to_coco(img_id, b[:, :4], b[:, 4], s, px, py)

    ids = [im["id"] for im in images]
    res = {}
    for tag in ("float", "int8"):
        if a.save_dets and dets[tag]:
            json.dump(dets[tag], open(f"{a.save_dets}.{tag}.json", "w"))
        st = coco_eval(a.ann, dets[tag], ids, f"{tag} {a.size}", a.max_dets) if dets[tag] else None
        if st is not None:
            res[tag] = st
    print(json.dumps(res, indent=1))
    if a.out:
        json.dump(res, open(a.out, "w"), indent=1)


if __name__ == "__main__":
    main()
