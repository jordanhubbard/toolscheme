#!/usr/bin/env python3
"""Fetch the optional DuckDB C library for this host."""
import io
from pathlib import Path
import platform
import subprocess
import sys
import urllib.request
import zipfile

version, destination = sys.argv[1:]
system = platform.system()
arch = {"aarch64": "arm64", "arm64": "arm64", "x86_64": "amd64"}.get(platform.machine())
if system not in ("Darwin", "Linux") or not arch:
    sys.exit("DuckDB vendoring supports macOS and Linux on x86_64/arm64")
asset = "libduckdb-osx-universal.zip" if system == "Darwin" else "libduckdb-linux-" + arch + ".zip"
url = "https://github.com/duckdb/duckdb/releases/download/" + version + "/" + asset
with urllib.request.urlopen(url, timeout=60) as response:
    archive = zipfile.ZipFile(io.BytesIO(response.read()))
target = Path(destination)
target.mkdir(parents=True, exist_ok=True)
library = "libduckdb.dylib" if system == "Darwin" else "libduckdb.so"
for name in ("duckdb.h", library):
    (target / name).write_bytes(archive.read(name))
if system == "Darwin":
    subprocess.run(["install_name_tool", "-id", "@rpath/libduckdb.dylib", str(target / library)], check=True)
    subprocess.run(["codesign", "--force", "--sign", "-", str(target / library)], check=True)
print("Fetched " + version + " " + asset)
