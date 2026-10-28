"""Build a relocatable Windows x64 distribution from a Release build and OCR deps.

No downloads or installation occur here: supply the verified CPython archive,
installed site-packages, two local model directories, Qt tools and MSVC redist.
The output must be a new directory, so an existing package is never overwritten.
"""
import argparse
import hashlib
import importlib.metadata
import json
from pathlib import Path
import shutil
import subprocess
import sys
import xml.etree.ElementTree as ET
import zipfile
from utf8_runtime import patch_runtime

PYTHON_ARCHIVE_SHA256 = "4acbed6dd1c744b0376e3b1cf57ce906f9dc9e95e68824584c8099a63025a3c3"
REPO = Path(__file__).resolve().parents[2]


def sha256(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def copy_tree(source, destination):
    shutil.copytree(source, destination, ignore=shutil.ignore_patterns("__pycache__", "*.pyc", "*.pyo"))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--release", type=Path, default=REPO / "build/Release")
    parser.add_argument("--site-packages", type=Path, default=REPO / "build/Release/ocr/venv/Lib/site-packages")
    parser.add_argument("--models", type=Path, default=REPO / "build/Release/ocr/cache/official_models")
    parser.add_argument("--python-archive", type=Path, default=REPO / "build/packaging-cache/python-3.12.10-embed-amd64.zip")
    parser.add_argument("--qt-bin", type=Path, default=Path("C:/Qt/Qt5.14.2/5.14.2/msvc2017_64/bin"))
    parser.add_argument("--opencv-bin", type=Path, default=Path("C:/openCV/opencv/build/x64/vc16/bin"))
    parser.add_argument("--manifest-tool", type=Path, default=Path("C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64/mt.exe"))
    parser.add_argument("--vc-redist", type=Path, default=Path("C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Redist/MSVC/14.44.35112/x64"))
    parser.add_argument("--verifier", type=Path, default=REPO / "build/ocr_package_test/Release/ocr_package_test.exe")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--zip", action="store_true", help="Also create a distributable ZIP")
    args = parser.parse_args()
    output = args.output.resolve()
    if output.exists() or output.with_suffix(".zip").exists():
        parser.error("Output directory/ZIP already exists; choose a new name")
    for path in (args.release / "gauge_reader_gui.exe", args.release / "gauge_reader_service.exe",
                 args.site_packages / "paddle", args.site_packages / "paddleocr", args.verifier,
                 args.qt_bin / "windeployqt.exe", args.vc_redist / "Microsoft.VC143.CRT", args.python_archive, args.manifest_tool):
        if not path.exists():
            parser.error(f"Required input missing: {path}")
    if sha256(args.python_archive) != PYTHON_ARCHIVE_SHA256:
        parser.error("CPython archive SHA256 differs from the official Python SPDX checksum")
    model_names = {"detection": "PP-OCRv5_server_det", "recognition": "PP-OCRv5_server_rec"}
    for name in model_names.values():
        for filename in ("inference.json", "inference.pdiparams", "inference.yml"):
            if not (args.models / name / filename).is_file():
                parser.error(f"Missing local model: {name}/{filename}")
    packages = sorted(({"name": dist.metadata["Name"], "version": dist.version}
                       for dist in importlib.metadata.distributions(path=[str(args.site_packages)])), key=lambda item: item["name"].lower())
    versions = {item["name"].lower(): item["version"] for item in packages}
    if versions.get("paddleocr") != "3.7.0" or versions.get("paddlepaddle") != "3.3.1":
        parser.error("Expected paddleocr 3.7.0 and paddlepaddle 3.3.1 in the supplied Python 3.12 site-packages")

    output.mkdir(parents=True)
    print(f"Building {output}", flush=True)
    # Only program/runtime files are selected: no device database, user settings or credentials.
    for filename in ("gauge_reader_gui.exe", "gauge_reader_service.exe"):
        shutil.copy2(args.release / filename, output / filename)
    for path in args.release.glob("*.dll"):
        shutil.copy2(path, output / path.name)
    ffmpeg = list(args.opencv_bin.glob("opencv_videoio_ffmpeg*_64.dll"))
    if not ffmpeg:
        raise FileNotFoundError("OpenCV FFmpeg video backend is required for camera streams")
    for path in ffmpeg:
        shutil.copy2(path, output / path.name)
    shutil.copy2(args.verifier, output / "ocr_verify.exe")
    subprocess.run([str(args.qt_bin / "windeployqt.exe"), "--release", "--no-translations", "--no-compiler-runtime",
                    str(output / "gauge_reader_gui.exe")], check=True)
    (output / "qt.conf").write_text("[Paths]\nPlugins=.\n", encoding="ascii")

    ocr = output / "ocr"
    runtime = ocr / "runtime"
    runtime.mkdir(parents=True)
    with zipfile.ZipFile(args.python_archive) as archive:
        archive.extractall(runtime)
    patch_runtime(runtime, args.manifest_tool)
    site_packages = runtime / "Lib/site-packages"
    copy_tree(args.site_packages, site_packages)
    # Explicit relative paths isolate the interpreter from registry, PATH and PYTHONHOME.
    (runtime / "python312._pth").write_text("python312.zip\n.\nLib/site-packages\n..\nimport site\n", encoding="ascii")
    for group in ("Microsoft.VC143.CRT", "Microsoft.VC143.OpenMP"):
        source = args.vc_redist / group
        if not source.is_dir():
            raise FileNotFoundError(f"MSVC runtime group missing: {source}")
        for path in source.glob("*.dll"):
            for destination in (output, runtime):
                shutil.copy2(path, destination / path.name)
    for filename in ("paddle_scale_ocr.py", "requirements.txt"):
        shutil.copy2(REPO / "tools/ocr" / filename, ocr / filename)
    config = {"ocr_version": "PP-OCRv5"}
    for kind, name in model_names.items():
        target = ocr / "models" / kind
        copy_tree(args.models / name, target)
        config[f"{kind}_model_name"] = name
        config[f"{kind}_model_dir"] = f"models/{kind}"
    (ocr / "offline.json").write_text(json.dumps(config, indent=2), encoding="utf-8")
    validation = ocr / "validation"
    validation.mkdir()
    shutil.copy2(REPO / "tools/packaging/verify_python.py", validation / "verify_python.py")
    shutil.copy2(REPO / "tools/packaging/verify-offline.ps1", output / "verify-offline.ps1")
    subprocess.run([str(runtime / "python.exe"), "-B", str(REPO / "tools/auto_scale_test/render_fixture.py"), str(validation / "dial.png")], check=True)

    licenses = output / "licenses"
    licenses.mkdir()
    shutil.copy2(runtime / "LICENSE.txt", licenses / "Python-LICENSE.txt")
    (licenses / "Python-packages.json").write_text(json.dumps(packages, indent=2), encoding="utf-8")
    for source, pattern, prefix in ((args.qt_bin.parent.parent / "Src", "LICENSE.*", "Qt-"),
                                     (args.opencv_bin.parents[3], "LICENSE*.txt", "OpenCV-"),
                                     (Path("C:/Program Files/MySQL/MySQL Server 8.0"), "LICENSE", "MySQL-")):
        if source.is_dir():
            for path in source.glob(pattern):
                if path.is_file():
                    shutil.copy2(path, licenses / (prefix + path.name))
    (licenses / "THIRD-PARTY.txt").write_text(
        "Python: PSF license, see Python-LICENSE.txt.\n"
        "Python package licenses/notices are retained in ocr/runtime/Lib/site-packages, including *.dist-info directories.\n"
        "OCR model license/notices are retained with ocr/models.\n"
        "Qt/OpenCV/MySQL and Microsoft redistributable libraries retain their respective licenses.\n", encoding="utf-8")
    (output / "START-HERE.txt").write_text(
        "GaugeReader offline package - Windows 10 (1903+) / Windows 11 x64\n\n"
        "Extract the ENTIRE archive into a short path, then run gauge_reader_gui.exe.\n"
        "Do not copy only the EXE. Keep ocr/runtime, ocr/models and Qt plugins.\n"
        "No Python installation, model download or internet connection is needed for OCR.\n"
        "Camera access and database connection still require the normal application configuration.\n"
        "Offline acceptance test: powershell -NoProfile -ExecutionPolicy Bypass -File .\\verify-offline.ps1\n"
        "The package uses CPU inference. Inspect reports before deploying to other hardware.\n", encoding="utf-8")
    print("Hashing distribution...", flush=True)
    files = {str(path.relative_to(output)).replace("\\", "/"): {"sha256": sha256(path), "bytes": path.stat().st_size}
             for path in sorted(output.rglob("*")) if path.is_file()}
    manifest = {"format": 1, "target": "Windows 10 1903+ / Windows 11 x64 CPU", "python": "3.12.10-embed-amd64", "python_process_code_page": "UTF-8 (embedded manifest added during packaging)",
                "python_archive_sha256": PYTHON_ARCHIVE_SHA256, "python_archive_source": "https://www.python.org/ftp/python/3.12.10/python-3.12.10-embed-amd64.zip",
                "packages": packages, "files": files}
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    reports = output.parent / (output.name + "-sandbox-results")
    reports.mkdir()
    sandbox = ET.Element("Configuration")
    for name, value in (("Networking", "Disable"), ("VGpu", "Disable"), ("AudioInput", "Disable"), ("ClipboardRedirection", "Disable"), ("MemoryInMB", "8192")):
        ET.SubElement(sandbox, name).text = value
    folders = ET.SubElement(sandbox, "MappedFolders")
    for host, guest, read_only in ((output, "C:\\GaugeReaderPackage", True), (reports, "C:\\GaugeReaderReports", False)):
        folder = ET.SubElement(folders, "MappedFolder")
        for name, value in (("HostFolder", str(host)), ("SandboxFolder", guest), ("ReadOnly", str(read_only).lower())):
            ET.SubElement(folder, name).text = value
    logon = ET.SubElement(sandbox, "LogonCommand")
    ET.SubElement(logon, "Command").text = "powershell.exe -NoProfile -ExecutionPolicy Bypass -File C:\\GaugeReaderPackage\\verify-offline.ps1 -OutputDirectory C:\\GaugeReaderReports -SandboxGuest"
    ET.ElementTree(sandbox).write(output.parent / (output.name + ".wsb"), encoding="utf-8", xml_declaration=True)
    if args.zip:
        print("Compressing ZIP...", flush=True)
        # Keep a top-level folder so extraction doesn't scatter application files.
        shutil.make_archive(str(output), "zip", root_dir=output.parent, base_dir=output.name)
        archive_path = output.with_suffix(".zip")
        archive_path.with_suffix(".zip.sha256").write_text(f"{sha256(archive_path)}  {archive_path.name}\n", encoding="ascii")
    print(json.dumps({"package": str(output), "file_count": len(files), "bytes": sum(item["bytes"] for item in files.values())}))


if __name__ == "__main__":
    main()
