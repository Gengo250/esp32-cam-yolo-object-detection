import argparse
import time
from datetime import datetime
from pathlib import Path
import os
import cv2
from ultralytics import YOLO

os.environ.setdefault("QT_QPA_PLATFORM", "xcb")

parser = argparse.ArgumentParser()
parser.add_argument("--source", required=True)
parser.add_argument("--model", default="models/yolo26n.pt")
parser.add_argument("--imgsz", type=int, default=320)
parser.add_argument("--conf", type=float, default=0.35)
parser.add_argument("--save", action="store_true")
parser.add_argument("--output", default="")
parser.add_argument("--fps", type=float, default=15.0)
args = parser.parse_args()

model = YOLO(args.model)
capture = cv2.VideoCapture(args.source)

writer = None
previous_time = time.perf_counter()

if args.output:
    output_path = Path(args.output)
else:
    timestamp = datetime.now().strftime("%Y-%m-%d_%H-%M-%S")
    output_path = Path("results") / f"esp32_yolo_{timestamp}.mp4"

output_path.parent.mkdir(parents=True, exist_ok=True)

while capture.isOpened():
    success, frame = capture.read()

    if not success:
        continue

    result = model.predict(
        source=frame,
        imgsz=args.imgsz,
        conf=args.conf,
        verbose=False,
    )[0]

    annotated_frame = result.plot()

    current_time = time.perf_counter()
    fps = 1 / max(current_time - previous_time, 0.0001)
    previous_time = current_time

    cv2.putText(
        annotated_frame,
        f"FPS: {fps:.1f}",
        (15, 30),
        cv2.FONT_HERSHEY_SIMPLEX,
        0.8,
        (255, 255, 255),
        2,
        cv2.LINE_AA,
    )

    if args.save and writer is None:
        height, width = annotated_frame.shape[:2]

        writer = cv2.VideoWriter(
            str(output_path),
            cv2.VideoWriter_fourcc(*"mp4v"),
            args.fps,
            (width, height),
        )

        print(f"Gravando em: {output_path.resolve()}")

    if writer is not None:
        writer.write(annotated_frame)

    cv2.imshow("ESP32-CAM Object Detection", annotated_frame)

    key = cv2.waitKey(1) & 0xFF

    if key == ord("q"):
        break

capture.release()

if writer is not None:
    writer.release()
    print(f"Vídeo salvo em: {output_path.resolve()}")

cv2.destroyAllWindows()
