"""Process-protocol fixture; never used by the application deployment."""
import argparse
import json
import math
import os
import time
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument("--image")
parser.add_argument("--output")
args = parser.parse_args()
mode = os.environ.get("GAUGE_OCR_FIXTURE_MODE", "success")
if mode == "sleep":
    time.sleep(30)
if mode == "invalid":
    Path(args.output).write_text("invalid json", encoding="utf-8")
else:
    tokens = []
    for index in range(1, 8):
        angle = math.radians(135 + 270 * index / 8)
        x, y = 400 + 210 * math.cos(angle), 400 + 210 * math.sin(angle)
        tokens.append(dict(text=str(index * 0.2), score=0.99,
                           polygon=[[x-8, y-8], [x+8, y-8], [x+8, y+8], [x-8, y+8]]))
    print("Library log deliberately outside the JSON response")
    if mode == "range":
        for text, x in (("0", 245), ("1.9", 555)):
            tokens.append(dict(text=text, score=.98,polygon=[[x-10,515],[x+10,515],[x+10,555],[x-10,555]]))
    replay = os.environ.get("GAUGE_OCR_FIXTURE_TOKENS")
    if replay:
        tokens = json.loads(Path(replay).read_text(encoding="utf-8"))["tokens"]
        full_height = os.environ.get("GAUGE_OCR_FIXTURE_FULL_HEIGHT")
        if full_height:
            import struct
            height = struct.unpack(">I", Path(args.image).read_bytes()[20:24])[0]
            for token in tokens:
                token["polygon"] = [[x, y-(int(full_height)-height)] for x, y in token["polygon"]]
    Path(args.output).write_text(json.dumps(dict(tokens=tokens)), encoding="utf-8")
