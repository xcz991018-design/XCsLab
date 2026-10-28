"""Preserve CPython's manifest and opt its process into UTF-8 native file APIs."""
from pathlib import Path
import subprocess
import xml.etree.ElementTree as ET


def patch_runtime(runtime: Path, manifest_tool: Path):
    asm1 = "urn:schemas-microsoft-com:asm.v1"
    asm3 = "urn:schemas-microsoft-com:asm.v3"
    utf8 = "http://schemas.microsoft.com/SMI/2019/WindowsSettings"
    ET.register_namespace("", asm1)
    ET.register_namespace("asmv3", asm3)
    for name in ("python.exe", "pythonw.exe"):
        executable = runtime / name
        manifest = runtime / (name + ".manifest-source.xml")
        subprocess.run([str(manifest_tool), "-nologo", f"-inputresource:{executable};#1", f"-out:{manifest}"], check=True)
        tree = ET.parse(manifest)
        application = tree.getroot().find(f"{{{asm3}}}application")
        if application is None:
            application = ET.SubElement(tree.getroot(), f"{{{asm3}}}application")
        settings = application.find(f"{{{asm3}}}windowsSettings")
        if settings is None:
            settings = ET.SubElement(application, f"{{{asm3}}}windowsSettings")
        code_page = settings.find(f"{{{utf8}}}activeCodePage")
        if code_page is None:
            code_page = ET.SubElement(settings, f"{{{utf8}}}activeCodePage")
        code_page.text = "UTF-8"
        tree.write(manifest, encoding="utf-8", xml_declaration=True)
        subprocess.run([str(manifest_tool), "-nologo", "-manifest", str(manifest), f"-outputresource:{executable};#1"], check=True)


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument("runtime", type=Path)
    parser.add_argument("mt", type=Path)
    args = parser.parse_args()
    patch_runtime(args.runtime.resolve(), args.mt.resolve())
