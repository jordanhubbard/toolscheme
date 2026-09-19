#!/usr/bin/env python3
"""Produce a relocatable installation archive, with no optional dependencies."""
import hashlib
from pathlib import Path
import platform
import shutil
import subprocess
import tarfile
import tempfile

root = Path(__file__).resolve().parent.parent
version = (root / "VERSION.txt").read_text().strip()
system = {"Darwin": "macos", "Linux": "linux"}[platform.system()]
arch = {"aarch64": "arm64", "arm64": "arm64", "x86_64": "x86_64"}[platform.machine()]
name = "toolscheme-" + version + "-" + system + "-" + arch
out = root / "dist"
out.mkdir(exist_ok=True)
with tempfile.TemporaryDirectory() as directory:
    stage = Path(directory) / name
    subprocess.run(["make", "install", "PREFIX=" + str(stage)], cwd=root, check=True)
    shutil.copy2(root / "README.md", stage)
    shutil.copytree(root / "docs", stage / "docs")
    archive = out / (name + ".tar.gz")
    with tarfile.open(archive, "w:gz") as bundle:
        bundle.add(stage, arcname=name)
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    (out / (name + ".sha256")).write_text(digest + "  " + archive.name + "\n")
print(archive)
