# SPDX-License-Identifier: AGPL-3.0-or-later
# /// script
# dependencies = ["numpy", "pillow"]
# ///
"""Convert packed RAW12 frames from the VI to RGB PNGs at half size.

Reference front end: each 2x2 Bayer group gives one RGB pixel (the two
greens are averaged), then black level, grey-world white balance and
gamma 2.2.

    uv run tools/raw2rgb.py WIDTH HEIGHT frame.raw... [-o outdir] [--bayer BGGR]
"""
import argparse
import os

import numpy as np
from PIL import Image


def unpack(path, w, h):
    """Return the 12-bit pixels of a packed RAW12 frame (2 px in 3 bytes)."""
    stride = (w * 3 // 2 + 15) // 16 * 16
    b = np.fromfile(path, np.uint8)[: stride * h].reshape(h, stride)
    b = b[:, : w * 3 // 2].reshape(h, w // 2, 3).astype(np.uint16)
    px = np.empty((h, w), np.uint16)
    px[:, 0::2] = (b[..., 0] << 4) | (b[..., 2] & 0xF)
    px[:, 1::2] = (b[..., 1] << 4) | (b[..., 2] >> 4)
    return px


def to_rgb(raw, bayer, black):
    planes = {}
    for i, c in enumerate(bayer):
        planes.setdefault(c, []).append(raw[i // 2 :: 2, i % 2 :: 2].astype(np.float32))
    rgb = np.stack([np.mean(planes[c], axis=0) for c in "RGB"], axis=-1) - black
    rgb = np.clip(rgb, 0, None)
    rgb *= rgb[..., 1].mean() / np.maximum(rgb.reshape(-1, 3).mean(axis=0), 1e-3)
    rgb /= max(np.percentile(rgb, 99.5), 1e-3)
    return (np.clip(rgb, 0, 1) ** (1 / 2.2) * 255 + 0.5).astype(np.uint8)


ap = argparse.ArgumentParser()
ap.add_argument("width", type=int)
ap.add_argument("height", type=int)
ap.add_argument("frames", nargs="+")
ap.add_argument("--bayer", default="BGGR")
ap.add_argument("--black", type=float, default=64 * 4, help="12-bit scale")
ap.add_argument("-o", "--outdir", default=".")
a = ap.parse_args()

os.makedirs(a.outdir, exist_ok=True)
for f in a.frames:
    rgb = to_rgb(unpack(f, a.width, a.height), a.bayer, a.black)
    out = os.path.join(a.outdir, os.path.basename(f).rsplit(".", 1)[0] + ".png")
    Image.fromarray(rgb).save(out)
print(f"{len(a.frames)} frames -> {a.outdir}")
