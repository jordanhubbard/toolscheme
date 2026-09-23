#!/usr/bin/env python3
"""Exercise merges, repeat installs, backups, and failure without data loss."""
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import tomllib
import unittest

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "scripts/configure-hooks.py"
spec = importlib.util.spec_from_file_location("configure_hooks", SCRIPT)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class ConfigureHooksTest(unittest.TestCase):
    def test_claude_preserves_settings_and_updates_prefix(self):
        original = {"permissions": {"allow": ["Bash(ls)"]}, "hooks": {
            "PreToolUse": [{"matcher": "*", "hooks": [
                {"type": "command", "command": "custom-hook"}]}]}}
        first = module.claude_config(json.dumps(original), "/old/share/toolscheme/hooks/observe.sh")
        updated = module.claude_config(first, "'/new prefix/share/toolscheme/hooks/observe.sh'")
        config = json.loads(updated)
        self.assertEqual(config["permissions"], original["permissions"])
        self.assertEqual(config["hooks"]["PreToolUse"][0], original["hooks"]["PreToolUse"][0])
        self.assertEqual(len(config["hooks"]["PreToolUse"]), 2)
        self.assertEqual(module.claude_config(updated, "'/new prefix/share/toolscheme/hooks/observe.sh'"), updated)

    def test_codex_preserves_comments_and_hooks(self):
        source = '# keep this comment\nmodel = "example"\n[[hooks.PreToolUse]]\nmatcher = "*"\n[[hooks.PreToolUse.hooks]]\ntype = "command"\ncommand = "custom-hook"\n'
        updated = module.codex_config(source, "'/old path/observe.sh'")
        self.assertTrue(updated.startswith(source))
        self.assertEqual(module.codex_config(updated, "'/old path/observe.sh'"), updated)
        updated = module.codex_config(updated, "/new/observe.sh")
        hooks = tomllib.loads(updated)["hooks"]["PreToolUse"]
        self.assertEqual(len(hooks), 2)
        self.assertEqual(hooks[1]["hooks"][0]["command"], "/new/observe.sh")
        self.assertEqual(module.codex_config(source, "custom-hook"), source)

    def test_files_and_invalid_configuration(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            claude = base / "claude/settings.json"
            codex = base / "codex/config.toml"
            claude.parent.mkdir()
            codex.parent.mkdir()
            claude.write_text('{"permissions": {"allow": []}}\n')
            original = claude.read_text()
            codex.write_text('invalid = [')
            args = ["python3", str(SCRIPT), "--hook", str(base / "share/toolscheme/hooks/observe.sh"),
                    "--claude-dir", str(claude.parent), "--codex-dir", str(codex.parent)]
            result = subprocess.run(args, capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(claude.read_text(), original)
            codex.unlink()
            subprocess.run(args, check=True, capture_output=True)
            backups = list(claude.parent.glob("*.toolscheme-backup-*"))
            self.assertEqual(len(backups), 1)
            self.assertEqual(backups[0].read_text(), original)
            before = (claude.read_bytes(), codex.read_bytes())
            subprocess.run(args, check=True, capture_output=True)
            self.assertEqual(before, (claude.read_bytes(), codex.read_bytes()))
            self.assertEqual(len(list(claude.parent.glob("*.toolscheme-backup-*"))), 1)


if __name__ == "__main__":
    unittest.main()
