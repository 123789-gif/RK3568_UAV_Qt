#!/usr/bin/env python3
"""Run in a separate conversion environment, not on RK3568. See drone.json for pinned versions."""
import argparse, json, hashlib
from pathlib import Path
from ultralytics import YOLO
from rknn.api import RKNN
import onnx
p=argparse.ArgumentParser();p.add_argument("weights");p.add_argument("output");a=p.parse_args()
out=Path(a.output);out.mkdir(parents=True,exist_ok=True)
m=YOLO(a.weights)
assert len(m.names)==1 and str(m.names[0]).lower()=="drone", "require single-class drone weights"
onnx_path=m.export(format="onnx",imgsz=640,batch=1,opset=12,simplify=False,dynamic=False)
g=onnx.load(onnx_path)
shape=[v.dim_value for v in g.graph.output[0].type.tensor_type.shape.dim]
assert len(g.graph.output)==1 and shape==[1,5,8400], shape
r=RKNN()
try:
    assert r.config(target_platform="rk3568",mean_values=[[0,0,0]],std_values=[[255,255,255]],optimization_level=3)==0
    assert r.load_onnx(model=onnx_path)==0
    assert r.build(do_quantization=False)==0
    assert r.export_rknn(str(out/"drone.rknn"))==0
finally:r.release()
print("Exported:",out/"drone.rknn")
print("SHA256:",hashlib.sha256((out/"drone.rknn").read_bytes()).hexdigest())
print("Update drone.json and SHA256SUMS together before deployment.")
