#!/usr/bin/env python3
"""Validate an installed tree after relocation, outside the source checkout."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import contextlib

root = Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory(prefix="toolscheme install ") as tmp:
    base = Path(tmp)
    prefix = base / "original"
    subprocess.run(["make", "install", "PREFIX=" + str(prefix)], cwd=root, check=True, stdout=subprocess.DEVNULL)
    relocated = base / "relocated"
    prefix.rename(relocated)
    env = dict(os.environ, TOOLSCHEME_STATE=str(base / "state"))
    env.pop("TOOLSCHEME_LIB", None)
    binary = relocated / "bin/toolscheme"
    def call(*args, data=None):
        result = subprocess.run([str(binary), *args], cwd=base, env=env, input=data,
                                text=True, capture_output=True, timeout=10)
        assert result.returncode == 0, result.stderr
        assert "cannot read" not in result.stderr, result.stderr
        return result.stdout
    # An installed DuckDB build must not quietly resolve its library from the
    # source checkout. Temporarily hide vendored copies during every smoke test.
    hidden = []
    @contextlib.contextmanager
    def without_vendor():
        try:
            for name in ("libduckdb.dylib", "libduckdb.so"):
                source = root / "vendor/duckdb" / name
                if source.exists():
                    moved = source.with_name(name + ".install-test")
                    source.rename(moved)
                    hidden.append((source, moved))
            yield
        finally:
            for source, moved in hidden:
                moved.rename(source)
    with without_vendor():
        assert call("-e", "(+ 20 22)").strip() == "42"
        assert call("--version").strip() == "toolscheme " + (root / "VERSION.txt").read_text().strip()
    response = json.loads(call("mcp", data=json.dumps(dict(jsonrpc="2.0", id=1, method="tools/list")) + "\n"))
    assert any(t["name"] == "search-read" for t in response["result"]["tools"]), response
    assert "durable" in call("learn", "--help") or "Git" in call("learn", "--help")
    hook = relocated / "share/toolscheme/hooks/observe.sh"
    event = dict(hook_event_name="PreToolUse", tool_name="Bash", tool_input=dict(command="echo hello"), session_id="install", tool_use_id="1")
    result = subprocess.run(["sh", str(hook)], cwd=base, env=env, input=json.dumps(event), text=True, capture_output=True, timeout=10)
    assert result.returncode == 0, result.stderr
    record = json.loads((base / "state/observations.jsonl").read_text())
    assert record["command"] == "echo hello", record
print("Relocated installation, CLI, MCP, learning command, and hook passed.")
