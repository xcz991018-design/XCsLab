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
parser.add_argument("--worker", action="store_true")
args = parser.parse_args()

def respond(args):
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
        if mode == "endpoints_only":
            tokens.clear()
        if mode in ("range", "endpoints_only"):
            for text, x in (("0", 245), ("1.9", 555)):
                tokens.append(dict(text=text, score=.98,polygon=[[x-10,515],[x+10,515],[x+10,555],[x-10,555]]))
        replay = os.environ.get("GAUGE_OCR_FIXTURE_TOKENS")
        if replay:
            tokens = json.loads(Path(replay).read_text(encoding="utf-8"))["tokens"]
        # Fixture polygons are in the full square rectified frame. Emulate OCR
        # returning local coordinates for the same dial/liquid crop as production.
        import struct
        width, height = struct.unpack(">II", Path(args.image).read_bytes()[16:24])
        full_size = int(os.environ.get("GAUGE_OCR_FIXTURE_FULL_HEIGHT", "800"))
        left = max(0, (full_size-width+1)//2)
        top = max(0, full_size-height-max(0,left-1))
        for token in tokens:
            token["polygon"] = [[x-left, y-top] for x, y in token["polygon"]]
        Path(args.output).write_text(json.dumps(dict(tokens=tokens)), encoding="utf-8")

if args.worker:
    import sys
    for line in sys.stdin:
        request = json.loads(line)
        pending = request["output"] + ".pending"
        respond(argparse.Namespace(image=request["image"], output=pending))
        os.replace(pending, request["output"])
else:
    respond(args)
