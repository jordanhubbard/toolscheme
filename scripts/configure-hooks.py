#!/usr/bin/env python3
"""Merge observation hooks into user settings without external dependencies."""
import argparse
import json
import os
from pathlib import Path
import shlex
import stat
import tempfile
import tomllib

START = "# BEGIN toolscheme observation hook"
END = "# END toolscheme observation hook"


def claude_config(source, command):
    config = json.loads(source or "{}")
    hooks = config.setdefault("hooks", {})
    for event in ("PreToolUse", "PostToolUse"):
        entries = hooks.setdefault(event, [])
        if not isinstance(entries, list):
            raise ValueError(f"{event} hooks must be an array")
        found = False
        for entry in entries:
            if entry.get("matcher") != "*":
                continue
            for hook in entry.get("hooks", []):
                old = hook.get("command", "")
                words = shlex.split(old)
                if hook.get("type") == "command" and (old == command or (
                    len(words) == 1 and words[0].endswith("/share/toolscheme/hooks/observe.sh")
                )):
                    hook["command"] = command
                    found = True
        if not found:
            entries.append({"matcher": "*", "hooks": [
                {"type": "command", "command": command, "timeout": 5}]})
    return json.dumps(config, indent=2, ensure_ascii=False) + "\n"


def codex_config(source, command):
    tomllib.loads(source)
    lines = source.splitlines(keepends=True)
    starts = [i for i, line in enumerate(lines) if line.strip() == START]
    ends = [i for i, line in enumerate(lines) if line.strip() == END]
    if starts or ends:
        if len(starts) != 1 or len(ends) != 1 or starts[0] >= ends[0]:
            raise ValueError("invalid toolscheme managed block")
        del lines[starts[0]:ends[0] + 1]
    base = "".join(lines)
    config = tomllib.loads(base)
    for entry in config.get("hooks", {}).get("PreToolUse", []):
        if entry.get("matcher") == "*" and any(
            hook.get("type") == "command" and hook.get("command") == command
            for hook in entry.get("hooks", [])
        ):
            return base
    result = base.rstrip("\n") + ("\n\n" if base.strip() else "") + (
        f'{START}\n[[hooks.PreToolUse]]\nmatcher = "*"\n'
        f'[[hooks.PreToolUse.hooks]]\ntype = "command"\n'
        f'command = {json.dumps(command, ensure_ascii=False)}\n{END}\n'
    )
    # Inline TOML hook tables cannot be extended by an array-of-tables declaration.
    # Refuse those layouts instead of risking unrelated user settings.
    tomllib.loads(result)
    return result


def write_config(path, original, updated):
    if original == updated:
        print(f"hooks already configured: {path}")
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    mode = stat.S_IMODE(path.stat().st_mode) if path.exists() else 0o600
    if path.exists():
        fd, backup = tempfile.mkstemp(prefix=path.name + ".toolscheme-backup-", dir=path.parent)
        with os.fdopen(fd, "w") as stream:
            stream.write(original)
        print(f"backup: {backup}")
    fd, temporary = tempfile.mkstemp(prefix=path.name + ".", dir=path.parent)
    try:
        with os.fdopen(fd, "w") as stream:
            stream.write(updated)
        os.chmod(temporary, mode)
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)
    print(f"configured hooks: {path}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--hook", required=True)
    parser.add_argument("--claude-dir", required=True)
    parser.add_argument("--codex-dir", required=True)
    args = parser.parse_args()
    command = shlex.quote(str(Path(args.hook).resolve()))
    pending = []
    # Validate both before changing either configuration.
    for path, merge in ((Path(args.claude_dir) / "settings.json", claude_config),
                        (Path(args.codex_dir) / "config.toml", codex_config)):
        path = path.expanduser().resolve()
        original = path.read_text() if path.exists() else ""
        try:
            updated = merge(original, command)
        except (ValueError, TypeError, AttributeError) as error:
            parser.exit(1, f"Cannot configure {path}: {error}\nNo configuration files changed.\n")
        pending.append((path, original, updated))
    for item in pending:
        write_config(*item)


if __name__ == "__main__":
    main()
