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
systems = {"Darwin": "macos", "Linux": "linux", "FreeBSD": "freebsd"}
architectures = {"aarch64": "arm64", "arm64": "arm64", "x86_64": "x86_64", "amd64": "x86_64"}
if platform.system() not in systems:
    raise SystemExit("unsupported system for packaging: " + platform.system())
if platform.machine() not in architectures:
    raise SystemExit("unsupported architecture for packaging: " + platform.machine())
system = systems[platform.system()]
arch = architectures[platform.machine()]

# This Makefile is GNU-flavoured, and on a BSD `make` is not GNU make. Packaging
# died with a parse error rather than anything that named the cause.
make = shutil.which("gmake") or "make"
name = "toolscheme-" + version + "-" + system + "-" + arch
out = root / "dist"
out.mkdir(exist_ok=True)
with tempfile.TemporaryDirectory() as directory:
    stage = Path(directory) / name
    subprocess.run([make, "install", "CONFIGURE_HOOKS=0", "PREFIX=" + str(stage)], cwd=root, check=True)
    shutil.copy2(root / "README.md", stage)
    shutil.copytree(root / "docs", stage / "docs")
    archive = out / (name + ".tar.gz")
    with tarfile.open(archive, "w:gz") as bundle:
        bundle.add(stage, arcname=name)
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    (out / (name + ".sha256")).write_text(digest + "  " + archive.name + "\n")
print(archive)
