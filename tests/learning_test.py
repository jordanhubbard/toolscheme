#!/usr/bin/env python3
"""Exercise real Git repositories, two independent hosts, and actual CLI startup."""
import json
import fcntl
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "scripts/learning.py"


class LearningTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="toolscheme learning ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.remote = self.root / "remote.git"
        subprocess.run(["git", "init", "--bare", "-b", "main", str(self.remote)], check=True, capture_output=True)
        self.a, self.b = self.root / "a", self.root / "b"
        for state in (self.a, self.b):
            self.call(state, "init", "--remote", str(self.remote), "--host", state.name)

    def call(self, state, *args, ok=True):
        result = subprocess.run([sys.executable, str(SCRIPT), "--state", str(state), *args], capture_output=True, text=True, timeout=60)
        if ok:
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        else:
            self.assertNotEqual(result.returncode, 0)
        return result

    def snapshot(self, state):
        return json.loads((state / "learning-snapshot.json").read_text())

    def test_convergence_offline_recovery_and_revocation(self):
        self.call(self.a, "record", "read-bounds", "Read only the relevant range.")
        self.call(self.b, "record", "wait-events", "Wait for observable completion.")
        # Both hosts commit while disconnected, then reconcile independently
        # created histories without losing either side's records.
        unavailable = self.root / "offline.git"
        self.remote.rename(unavailable)
        for state in (self.a, self.b):
            self.call(state, "sync", ok=False)
            self.assertTrue(self.snapshot(state)["notes"])
        unavailable.rename(self.remote)
        self.call(self.a, "sync")
        self.call(self.b, "sync")
        self.call(self.a, "sync")
        self.assertEqual(self.snapshot(self.a), self.snapshot(self.b))
        self.assertEqual(len(self.snapshot(self.a)["notes"]), 2)
        before = subprocess.check_output(["git", "-C", str(self.a / "learning-git"), "rev-parse", "HEAD"])
        self.call(self.a, "sync")
        after = subprocess.check_output(["git", "-C", str(self.a / "learning-git"), "rev-parse", "HEAD"])
        self.assertEqual(before, after, "unchanged sync must not manufacture commits")
        self.call(self.b, "record", "read-bounds", "--disable")
        self.call(self.b, "sync")
        self.call(self.a, "sync")
        self.assertEqual(self.snapshot(self.a)["notes"], ["Wait for observable completion."])
        # Startup consumes data with no remote available and does not eval it.
        self.remote.rename(unavailable)
        result = subprocess.run([str(ROOT / "toolscheme"), "-e", "learning-notes"], cwd=self.root,
                                env=dict(os.environ, TOOLSCHEME_STATE=str(self.a)), capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("observable completion", result.stdout)
        result = subprocess.run([str(ROOT / "toolscheme"), "mcp"], cwd=self.root,
                                env=dict(os.environ, TOOLSCHEME_STATE=str(self.a)),
                                input=json.dumps(dict(jsonrpc="2.0", id=1, method="initialize")) + "\n",
                                capture_output=True, text=True, timeout=5)
        self.assertEqual(json.loads(result.stdout)["result"]["instructions"], "Wait for observable completion.")

    def test_incremental_complete_lines_and_private_inputs(self):
        event = dict(session="private-session", event="pre", command="sleep 55", cwd="/private/project", input="SECRET", at=1)
        log = self.a / "observations.jsonl"
        line = json.dumps(event)
        log.write_text(line + "\n" + line[:20])
        self.call(self.a, "sync")
        self.assertEqual(self.snapshot(self.a)["totals"]["pre"], 1)
        self.assertEqual(self.snapshot(self.a)["totals"]["fixed_waits"], 1)
        self.call(self.a, "sync")
        self.assertEqual(self.snapshot(self.a)["totals"]["pre"], 1)
        with log.open("a") as out:
            out.write(line[20:] + "\n")
        self.call(self.a, "sync")
        self.assertEqual(self.snapshot(self.a)["totals"]["pre"], 2)
        for path in (self.a / "learning-git/records").rglob("*.json"):
            text = path.read_text()
            for private in ("SECRET", "/private/project", "private-session", "sleep 55"):
                self.assertNotIn(private, text)
        self.call(self.b, "sync")
        self.assertEqual(self.snapshot(self.a), self.snapshot(self.b))

    def test_concurrent_pushes_and_process_lock(self):
        for state in (self.a, self.b):
            self.call(state, "record", state.name, "Advice from " + state.name)
        jobs = [subprocess.Popen([sys.executable, str(SCRIPT), "--state", str(state), "sync"],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                for state in (self.a, self.b)]
        for job in jobs:
            out, err = job.communicate(timeout=60)
            self.assertEqual(job.returncode, 0, out + err)
        for state in (self.a, self.b):
            self.call(state, "sync")
        self.assertEqual(self.snapshot(self.a), self.snapshot(self.b))
        self.assertEqual(len(self.snapshot(self.a)["notes"]), 2)
        with (self.a / "learning.lock").open("a") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            result = self.call(self.a, "sync", ok=False)
            self.assertIn("another learning operation", result.stderr)
        self.call(self.a, "sync")

    def test_snapshot_is_data_and_hook_delivers_once(self):
        note = '(begin (write-file "PWNED" "x"))'
        self.call(self.a, "record", "literal", note)
        self.call(self.a, "sync")
        request = dict(hook_event_name="SessionStart", session_id="test-start")
        env = dict(os.environ, TOOLSCHEME_STATE=str(self.a), TOOLSCHEME_LIB=str(ROOT / "lib"))
        command = [str(ROOT / "toolscheme"), str(ROOT / "hooks/observe.scm"), "--root", str(self.a), "--stdin", "--text"]
        result = subprocess.run(command, input=json.dumps(request), env=env, capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(note, json.loads(result.stdout)["hookSpecificOutput"]["additionalContext"])
        self.assertFalse((self.a / "PWNED").exists())
        result = subprocess.run(command, input=json.dumps(request), env=env, capture_output=True, text=True, timeout=5)
        self.assertEqual(result.stdout, "")
        (self.a / "learning-snapshot.json").write_text("bad JSON")
        result = subprocess.run([str(ROOT / "toolscheme"), "-e", "(+ 20 22)"], env=env, capture_output=True, text=True, timeout=5)
        self.assertEqual(result.stdout.strip(), "42")


if __name__ == "__main__":
    unittest.main()
