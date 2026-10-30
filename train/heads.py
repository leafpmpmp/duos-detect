# SPDX-License-Identifier: AGPL-3.0-or-later
"""Write NAME.outputs for each NAME.onnx: the head convs (box, then class) that feed the
per-stride Reshape -> Concat of an Ultralytics detect head."""
import sys

import onnx

for path in sys.argv[1:]:
    m = onnx.load(path)
    cons = {}
    for nd in m.graph.node:
        for i in nd.input:
            cons.setdefault(i, []).append(nd)
    outs = []
    for nd in m.graph.node:
        c = cons.get(nd.output[0], [])
        if nd.op_type == "Conv" and len(c) == 1 and c[0].op_type == "Reshape" and \
                any(x.op_type == "Concat" for x in cons.get(c[0].output[0], [])):
            outs.append(nd.output[0])
    outs.sort(key=lambda s: (s.split("/")[2].split(".")[0], s))
    assert len(outs) == 6, (path, outs)
    open(path[:-5] + ".outputs", "w").write(",".join(outs))
