# SPDX-License-Identifier: AGPL-3.0-or-later
"""Raw-camera front end as an ONNX graph, for the TPU.

Input: the packed RAW12 frame bytes as a 1-channel image
[1, 1, H, stride]. Each 3-byte group holds the 8 MSBs of two pixels and
then their LSBs. One strided convolution reads a BIN x BIN Bayer block
from the MSB bytes (it ignores the LSB bytes) and gives one RGB pixel.
Then: black level, white balance, normalise to 0..1, gamma, crop.

    python3 frontend.py onnx out.onnx [options]
    python3 frontend.py npz out_dir frame.raw...   # model inputs
    python3 frontend.py png out_dir frame.raw...   # same, as grey PNGs
    python3 frontend.py ref out.npz frame.raw...   # reference outputs
"""
import argparse
import os

import numpy as np

SENSOR_W, SENSOR_H = 2304, 1296


def stride_of(width):
    return (width * 3 // 2 + 15) // 16 * 16


def kernel(bayer, order, binning):
    """Conv weights [3, 1, binning, binning * 3 // 2] for one Bayer block."""
    kw = binning * 3 // 2
    w = np.zeros((3, 1, binning, kw), np.float32)
    for r in range(binning):
        for px in range(binning):
            col = px // 2 * 3 + px % 2  # MSB byte of this pixel
            c = bayer[(r % 2) * 2 + px % 2]
            w[order.index(c), 0, r, col] += 1
    # Average: each output channel sums its own samples
    return w / w.sum(axis=(1, 2, 3), keepdims=True)


def params(a):
    return dict(
        width=a.width, height=a.height, binning=a.binning,
        out_h=a.out_h, out_w=a.out_w, bayer=a.bayer, order=a.order,
        layout=a.layout,
        black=a.black, white=a.white, gains=tuple(a.gains), gamma=a.gamma,
    )


def crop_box(p):
    h, w = p["height"] // p["binning"], p["width"] // p["binning"]
    top, left = (h - p["out_h"]) // 2, (w - p["out_w"]) // 2
    return top, left


def reference(raw, p):
    """Same math as the ONNX graph, in float64. raw: [H, stride] uint8."""
    b = p["binning"]
    w = kernel(p["bayer"], p["order"], b)[:, 0]
    h_out, w_out = p["height"] // b, p["width"] // b
    kw = b * 3 // 2
    x = raw[: h_out * b, : w_out * kw].astype(np.float64)
    blocks = x.reshape(h_out, b, w_out, kw).transpose(0, 2, 1, 3)
    rgb = np.einsum("yxrc,orc->oyx", blocks, w)
    g = np.array(p["gains"])[:, None, None]
    rgb = (rgb - p["black"]) * g / (p["white"] - p["black"])
    rgb = np.clip(rgb, 0, 1) ** (1 / p["gamma"])
    top, left = crop_box(p)
    return rgb[:, top : top + p["out_h"], left : left + p["out_w"]][None]


def build(p):
    import onnx
    from onnx import TensorProto, helper, numpy_helper

    b = p["binning"]
    stride = stride_of(p["width"])
    w = kernel(p["bayer"], p["order"], b)
    g = np.array(p["gains"], np.float32)
    scale = g / (p["white"] - p["black"])
    # (conv - black) * scale  ==  conv * scale + (-black * scale)
    w = w * scale[:, None, None, None]
    bias = -p["black"] * scale
    top, left = crop_box(p)
    inits = [
        numpy_helper.from_array(w.astype(np.float32), "bin_w"),
        numpy_helper.from_array(bias.astype(np.float32), "bin_b"),
        numpy_helper.from_array(np.array(1 / p["gamma"], np.float32), "inv_gamma"),
        numpy_helper.from_array(np.array([top, left], np.int64), "crop_start"),
        numpy_helper.from_array(
            np.array([top + p["out_h"], left + p["out_w"]], np.int64), "crop_end"),
        numpy_helper.from_array(np.array([2, 3], np.int64), "crop_axes"),
    ]
    kw = b * 3 // 2
    h_out, w_out = p["height"] // b, p["width"] // b
    if p["layout"] == "conv":
        nodes = [helper.make_node("Conv", ["raw", "bin_w", "bin_b"], ["binned"],
                                  kernel_shape=[b, kw], strides=[b, kw])]
    else:
        # One channel per block row, so the TPU lanes work in parallel:
        # [1, 1, H, S] -> [1, H/b, b, S], grouped conv with 3 outputs per
        # group -> [1, H/b * 3, 1, W/b] -> [1, 3, H/b, W/b]
        inits[0] = numpy_helper.from_array(
            np.tile(w, (h_out, 1, 1, 1)).astype(np.float32), "bin_w")
        inits[1] = numpy_helper.from_array(
            np.tile(bias, h_out).astype(np.float32), "bin_b")
        inits += [
            numpy_helper.from_array(
                np.array([1, h_out, b, stride], np.int64), "rows_shape"),
            numpy_helper.from_array(
                np.array([1, h_out, 3, w_out], np.int64), "rgb_shape"),
        ]
        nodes = [
            helper.make_node("Reshape", ["raw", "rows_shape"], ["rows"]),
            helper.make_node("Conv", ["rows", "bin_w", "bin_b"], ["blocks"],
                             kernel_shape=[b, kw], strides=[b, kw], group=h_out),
            helper.make_node("Reshape", ["blocks", "rgb_shape"], ["blocks_rgb"]),
            helper.make_node("Transpose", ["blocks_rgb"], ["binned"],
                             perm=[0, 2, 1, 3]),
        ]
    nodes += [
        helper.make_node("Relu", ["binned"], ["pos"]),
        helper.make_node("Pow", ["pos", "inv_gamma"], ["gamma"]),
        helper.make_node("Min", ["gamma", "one"], ["clipped"]),
        helper.make_node("Slice", ["clipped", "crop_start", "crop_end", "crop_axes"],
                         ["image"]),
    ]
    inits.append(numpy_helper.from_array(np.array(1.0, np.float32), "one"))
    graph = helper.make_graph(
        nodes, "frontend",
        [helper.make_tensor_value_info("raw", TensorProto.FLOAT,
                                       [1, 1, p["height"], stride])],
        [helper.make_tensor_value_info("image", TensorProto.FLOAT,
                                       [1, 3, p["out_h"], p["out_w"]])],
        inits)
    model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 13)])
    model.ir_version = 8
    onnx.checker.check_model(model)
    return model


def load_raw(path, p):
    stride = stride_of(p["width"])
    return np.fromfile(path, np.uint8)[: stride * p["height"]].reshape(
        p["height"], stride)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["onnx", "npz", "png", "ref"])
    ap.add_argument("out")
    ap.add_argument("frames", nargs="*")
    ap.add_argument("--width", type=int, default=SENSOR_W)
    ap.add_argument("--height", type=int, default=SENSOR_H)
    ap.add_argument("--binning", type=int, default=4)
    ap.add_argument("--out-h", type=int, default=320)
    ap.add_argument("--out-w", type=int, default=576)
    ap.add_argument("--bayer", default="BGGR")
    ap.add_argument("--order", default="RGB", help="output channel order")
    ap.add_argument("--black", type=float, default=16, help="8-bit MSB scale")
    ap.add_argument("--white", type=float, default=255)
    ap.add_argument("--gains", type=float, nargs=3, default=[1, 1, 1],
                    metavar=("R", "G", "B"))
    ap.add_argument("--gamma", type=float, default=2.2)
    ap.add_argument("--layout", choices=["conv", "dw"], default="conv",
                    help="dw: grouped conv over block rows (slower: 10.9 vs 9.5 ms)")
    a = ap.parse_intermixed_args()
    p = params(a)

    if a.cmd == "onnx":
        import onnx
        onnx.save(build(p), a.out)
        print("wrote", a.out)
    elif a.cmd == "npz":
        os.makedirs(a.out, exist_ok=True)
        for f in a.frames:
            raw = load_raw(f, p).astype(np.float32)[None, None]
            name = os.path.basename(f).rsplit(".", 1)[0] + ".npz"
            np.savez(os.path.join(a.out, name), raw=raw)
        print(len(a.frames), "inputs ->", a.out)
    elif a.cmd == "png":
        import cv2
        os.makedirs(a.out, exist_ok=True)
        for f in a.frames:
            name = os.path.basename(f).rsplit(".", 1)[0] + ".png"
            cv2.imwrite(os.path.join(a.out, name), load_raw(f, p))
        print(len(a.frames), "PNG inputs ->", a.out)
    else:
        out = {os.path.basename(f).rsplit(".", 1)[0]: reference(load_raw(f, p), p)
               for f in a.frames}
        np.savez(a.out, **out)
        print(len(out), "references ->", a.out)


if __name__ == "__main__":
    main()
