import onnx

model = onnx.load("yolov5s_relu.onnx")
model.graph.input[0].type.tensor_type.shape.dim[2].dim_value = 272  # H
model.graph.input[0].type.tensor_type.shape.dim[3].dim_value = 480  # W
onnx.save(model, "yolov5s_relu.onnx")

