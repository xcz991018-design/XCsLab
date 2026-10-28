"""Compare four-point stability on the same photo under photometric changes.

Requires OpenCV/numpy and a built auto_scale_test runner. The runner's Qt/OpenCV
DLL directories must be on PATH. This measures stability, not real-photo truth.
"""
import argparse
import json
import subprocess
import time
from pathlib import Path

import cv2
import numpy as np


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--image", required=True)
    parser.add_argument("--runner", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    folder = Path(args.output)
    folder.mkdir(parents=True, exist_ok=True)
    source = cv2.imdecode(np.fromfile(args.image, dtype=np.uint8), cv2.IMREAD_COLOR)
    if source is None:
        raise ValueError("Cannot decode input image")
    cases = ((1, 0, 95), (1, -12, 85), (1, 12, 85), (.85, 10, 75), (1.1, -8, 65))
    rows = {mode: [] for mode in ("baseline", "refined")}
    timings = {mode: [] for mode in rows}
    for index, (contrast, offset, quality) in enumerate(cases):
        pixels = np.clip(source.astype(float)*contrast+offset, 0, 255).astype(np.uint8)
        image = folder / f"input-{index}.jpg"
        cv2.imencode(".jpg", pixels, [cv2.IMWRITE_JPEG_QUALITY, quality])[1].tofile(str(image))
        for mode in rows:
            prefix = folder / f"{mode}-{index}"
            started = time.perf_counter()
            completed = subprocess.run([args.runner, "--dial-baseline" if mode == "baseline" else "--dial",
                                        str(image), str(prefix)], capture_output=True)
            timings[mode].append((time.perf_counter()-started)*1000)
            rows[mode].append(json.loads(prefix.with_suffix(".json").read_text(encoding="utf-8"))
                              if completed.returncode == 0 else None)
    summary = {"image": str(Path(args.image).resolve()), "image_size": list(source.shape[1::-1]),
               "measurement": "photometric stability; not physical accuracy"}
    for mode, results in rows.items():
        positions = np.asarray([row["points"] for row in results if row])
        summary[mode] = {"success": len(positions), "variants": len(results),
                         "mean_process_ms": float(np.mean(timings[mode]))}
        if len(positions):
            median = np.median(positions, axis=0)
            summary[mode]["corner_jitter_rms_px"] = float(np.sqrt(np.mean(np.sum((positions-median)**2, axis=2))))
    (folder / "summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(json.dumps(summary))


if __name__ == "__main__":
    main()
