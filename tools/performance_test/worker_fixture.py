import json
import os
import sys
import time
from pathlib import Path

for line in sys.stdin:
    request = json.loads(line)
    if request["image"] == "sleep":
        time.sleep(30)
    if request["image"] == "crash":
        sys.exit(2)
    output = Path(request["output"])
    pending = output.with_suffix(".pending")
    pending.write_text(json.dumps({"pid": os.getpid()}))
    os.replace(pending, output)
