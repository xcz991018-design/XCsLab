"""Run with the shipped interpreter; require all non-system imports in the bundle."""
import argparse
import ctypes
import hashlib
from ctypes import wintypes
import importlib.util
import json
import os
import re
from pathlib import Path
import sys
import traceback


def loaded_modules():
    psapi = ctypes.WinDLL("psapi")
    kernel = ctypes.WinDLL("kernel32")
    kernel.GetCurrentProcess.restype = wintypes.HANDLE
    modules = (ctypes.c_void_p * 4096)()
    needed = wintypes.DWORD()
    process = kernel.GetCurrentProcess()
    psapi.EnumProcessModules.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD)]
    psapi.GetModuleFileNameExW.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.LPWSTR, wintypes.DWORD]
    if not psapi.EnumProcessModules(process, modules, ctypes.sizeof(modules), ctypes.byref(needed)):
        raise ctypes.WinError()
    result = []
    for module in modules[:needed.value // ctypes.sizeof(ctypes.c_void_p)]:
        buffer = ctypes.create_unicode_buffer(32768)
        if psapi.GetModuleFileNameExW(process, module, buffer, len(buffer)):
            result.append(str(Path(buffer.value).resolve()))
    return sorted(result)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    ocr = root / "ocr"
    report = {"passed": False, "package_root": str(root), "executable": sys.executable,
              "python_version": sys.version, "sys_path": sys.path,
              "network_policy": "Python audit blocks connect, DNS and datagram sends; not an OS network sandbox"}
    attempts = []

    def deny_network(event, values):
        if event in {"socket.connect", "socket.getaddrinfo", "socket.gethostbyname", "socket.gethostbyaddr", "socket.sendto", "socket.sendmsg"}:
            attempts.append(event)
            raise RuntimeError(f"Validation blocked network: {event}")

    sys.addaudithook(deny_network)
    try:
        manifest = json.loads((root / "manifest.json").read_text(encoding="utf-8"))
        for name, record in manifest["files"].items():
            path = (root / name).resolve()
            if not path.is_relative_to(root):
                raise AssertionError(f"Invalid manifest path: {name}")
            with path.open("rb") as stream:
                if hashlib.file_digest(stream, "sha256").hexdigest() != record["sha256"]:
                    raise AssertionError(f"Package checksum mismatch: {name}")
        report["verified_file_count"] = len(manifest["files"])
        if not Path(sys.executable).resolve().is_relative_to(ocr / "runtime"):
            raise AssertionError("Interpreter is not bundled")
        if ctypes.windll.kernel32.GetACP() != 65001:
            raise AssertionError("Bundled Python requires UTF-8 process code page (Windows 10 1903+)")
        report["native_code_page"] = 65001
        if any(not Path(item).resolve().is_relative_to(root) for item in sys.path if item):
            raise AssertionError("External Python search path")
        spec = importlib.util.spec_from_file_location("gauge_paddle_adapter", ocr / "paddle_scale_ocr.py")
        adapter = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(adapter)
        tokens = adapter.recognize(str(ocr / "validation" / "dial.png"))
        numeric = {token["text"] for token in tokens if token["score"] >= .8}
        if not {"0", "0.2", "0.4", "0.6", "0.8", "1"}.issubset(numeric):
            raise AssertionError(f"Missing fixture digits: {sorted(numeric)}")
        external = []
        for name, module in list(sys.modules.items()):
            path = getattr(module, "__file__", None)
            if path and not path.startswith("<") and not Path(path).resolve().is_relative_to(root):
                external.append({"module": name, "path": path})
        if external:
            raise AssertionError(f"Imports escaped bundle: {external}")
        dlls = loaded_modules()
        windows = Path(os.environ["SystemRoot"]).resolve()
        outside = [path for path in dlls if not Path(path).is_relative_to(root) and not Path(path).is_relative_to(windows)]
        # msvcp_win.dll is a Windows OS component, not the redistributable msvcp140.dll.
        vc_pattern = re.compile(r"^(?:msvcp|vcruntime|concrt|vcomp)[0-9]", re.I)
        external_vc = [path for path in dlls if vc_pattern.match(Path(path).name) and not Path(path).is_relative_to(root)]
        if outside or external_vc:
            raise AssertionError(f"External native dependencies: {outside + external_vc}")
        if attempts:
            raise AssertionError(f"Network was attempted: {attempts}")
        report.update(passed=True, recognized_numbers=sorted(numeric), tokens=tokens, loaded_native_modules=dlls)
    except Exception:
        report["error"] = traceback.format_exc()
    report["blocked_network_attempts"] = attempts
    Path(args.output).write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print("PASS: bundled Python, imports, DLLs, local models, no network attempts" if report["passed"] else report["error"])
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
