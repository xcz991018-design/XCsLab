"""Deploy a validated offline OCR bundle beside a CMake/qmake build.

The offline marker is published last so an incomplete copy is never selected.
This copies the portable runtime, not a development virtual environment.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bundle", type=Path, required=True, help="Extracted offline application package")
    parser.add_argument("--destination", type=Path, required=True, help="Target ocr directory (qmake: build/ocr)")
    args = parser.parse_args()
    source = args.bundle.resolve() / "ocr"
    target = args.destination.resolve()
    if source == target or source.is_relative_to(target) or target.is_relative_to(source):
        parser.error("Source and destination must be separate directories")
    if (target / "offline.json").exists():
        parser.error("Destination already contains an offline bundle; choose a fresh directory for upgrades")
    manifest = json.loads((source.parent / "manifest.json").read_text(encoding="utf-8"))
    records = {name: record for name, record in manifest["files"].items() if name.startswith("ocr/")}
    required = {"ocr/offline.json", "ocr/runtime/python.exe", "ocr/paddle_scale_ocr.py"}
    for kind in ("detection", "recognition"):
        required.update(f"ocr/models/{kind}/{name}" for name in ("inference.json", "inference.pdiparams", "inference.yml"))
    if not required.issubset(records):
        parser.error("Incomplete source bundle manifest")
    target.mkdir(parents=True, exist_ok=True)
    print(f"Deploying portable OCR to {target}", flush=True)
    for name, record in records.items():
        if name == "ocr/offline.json":
            continue
        relative = Path(name).relative_to("ocr")
        if relative.is_absolute() or ".." in relative.parts:
            raise ValueError(f"Invalid manifest entry: {name}")
        origin, destination = source / relative, target / relative
        with origin.open("rb") as stream:
            if hashlib.file_digest(stream, "sha256").hexdigest() != record["sha256"]:
                raise ValueError(f"Source checksum mismatch: {name}")
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(origin, destination)
    marker = (source / "offline.json").read_bytes()
    if hashlib.sha256(marker).hexdigest() != records["ocr/offline.json"]["sha256"]:
        raise ValueError("Offline configuration checksum mismatch")
    temporary = target / "offline.json.tmp"
    temporary.write_bytes(marker)
    temporary.replace(target / "offline.json")
    print("PASS: portable runtime, local models and offline configuration deployed", flush=True)


if __name__ == "__main__":
    main()
