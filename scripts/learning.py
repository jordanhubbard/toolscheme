#!/usr/bin/env python3
"""Local learning journal and optional Git transport. Never run by a tool hook.

Only immutable JSON records travel through Git. The hot path reads a bounded,
atomically replaced snapshot; it never runs this program or waits for a remote.
"""
import argparse
import collections
import contextlib
import fcntl
import hashlib
import json
import os
from pathlib import Path
import plistlib
import re
import subprocess
import sys
import tempfile
import time
import uuid

SCHEMA = 1
MAX_BATCH = 4 * 1024 * 1024
MAX_RECORD = 65536


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True) + "\n"


def atomic(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=".pending-", dir=str(path.parent))
    try:
        with os.fdopen(fd, "w") as out:
            out.write(canonical(value))
            out.flush()
            os.fsync(out.fileno())
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def read_json(path, default=None):
    try:
        if path.is_symlink() or path.stat().st_size > MAX_RECORD:
            return default
        return json.loads(path.read_text())
    except (OSError, ValueError):
        return default


def run(args, cwd=None, check=True):
    env = dict(os.environ, GIT_TERMINAL_PROMPT="0", GCM_INTERACTIVE="never")
    # Background jobs must not ask for a password or wait indefinitely for SSH.
    env.setdefault("GIT_SSH_COMMAND", "ssh -oBatchMode=yes -oConnectTimeout=10")
    result = subprocess.run(args, cwd=cwd, env=env, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=45)
    if check and result.returncode:
        raise RuntimeError("command failed: " + args[0] + " " + " ".join(args[1:3])
                           + " (exit " + str(result.returncode) + ")")
    return result


def git(repo, *args, check=True):
    # Shared data is never allowed to supply executable hooks or filters.
    return run(["git", "-c", "core.hooksPath=/dev/null", "-c", "commit.gpgsign=false",
                "-c", "core.autocrlf=false", *args], cwd=repo, check=check)


@contextlib.contextmanager
def locked(state):
    state.mkdir(parents=True, exist_ok=True)
    with (state / "learning.lock").open("a") as handle:
        try:
            fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise RuntimeError("another learning operation is running")
        yield


def configuration(state):
    config = read_json(state / "learning-config.json")
    if not isinstance(config, dict) or config.get("schema") != SCHEMA:
        raise RuntimeError("run toolscheme learn init --remote URL first")
    return config


def enqueue(state, config, record):
    record.update(schema=SCHEMA, host=config["host"])
    digest = hashlib.sha256(canonical(record).encode()).hexdigest()
    atomic(state / "learning-outbox" / (digest + ".json"), record)


def capture(state, config):
    """Incremental, complete-line ingestion. A retry writes identical object names.

    No raw command, path, input, response, or session identifier leaves this host.
    Session IDs are hashed; aggregates retain the evidence behind shared advice.
    """
    source = state / "observations.jsonl"
    if not source.exists():
        return
    checkpoint = read_json(state / "learning-cursor.json", {})
    with source.open("rb") as stream:
        st = os.fstat(stream.fileno())
        identity = str(st.st_dev) + ":" + str(st.st_ino)
        offset = checkpoint.get("offset", 0) if checkpoint.get("identity") == identity else 0
        if offset > st.st_size:
            offset = 0
        stream.seek(offset)
        data = stream.read(MAX_BATCH)
    end = data.rfind(b"\n") + 1
    if not end:
        return
    data = data[:end]
    sessions = {}
    rejected = 0
    for line in data.splitlines():
        try:
            event = json.loads(line)
            if not isinstance(event, dict):
                raise ValueError()
            session = hashlib.sha256(str(event.get("session", "unknown")).encode()).hexdigest()
            counts = sessions.setdefault(session, dict(pre=0, post=0, failures=0,
                                                       result_bytes=0, fixed_waits=0))
            kind = event.get("event")
            if kind in ("pre", "post"):
                counts[kind] += 1
            if kind == "post":
                counts["failures"] += event.get("ok") is False
                counts["result_bytes"] += max(0, int(event.get("bytes", 0)))
            command = event.get("command", "")
            if kind == "pre" and isinstance(command, str) and re.fullmatch(r"\s*sleep\s+\d+(?:\.\d+)?\s*", command):
                counts["fixed_waits"] += 1
        except (ValueError, TypeError):
            rejected += 1
    # One object per session per batch: bounded files, independent host writes,
    # and retries deduplicate without diffing or rewriting a growing JSON array.
    batch = hashlib.sha256((identity + ":" + str(offset)).encode() + data).hexdigest()
    for session, counts in sorted(sessions.items()):
        enqueue(state, config, dict(kind="session", session=session, batch=batch, **counts))
    atomic(state / "learning-cursor.json", dict(identity=identity, offset=offset + end,
                                               rejected=checkpoint.get("rejected", 0) + rejected))


def records(repo):
    directory = repo / "records"
    if directory.is_symlink():
        raise RuntimeError("records directory must not be a symlink")
    if not directory.exists():
        return
    for host in sorted(directory.iterdir()):
        if host.is_symlink() or not host.is_dir():
            continue
        for path in sorted(host.glob("*.json")):
            record = read_json(path)
            if isinstance(record, dict) and record.get("schema") == SCHEMA:
                yield record


def snapshot(state, repo):
    head = git(repo, "rev-parse", "--verify", "HEAD", check=False).stdout.strip()
    cached = read_json(state / "learning-snapshot.json")
    if cached is not None and head and read_json(state / "learning-snapshot-head.json") == head:
        return cached
    totals = collections.Counter()
    revisions = {}
    hosts = set()
    for record in records(repo):
        host = record.get("host")
        if isinstance(host, str):
            hosts.add(host)
        if record.get("kind") == "session":
            for key in ("pre", "post", "failures", "result_bytes", "fixed_waits"):
                value = record.get(key, 0)
                if isinstance(value, int) and 0 <= value <= 2**53:
                    totals[key] += value
        elif record.get("kind") == "steering":
            key, text = record.get("id"), record.get("text")
            stamp = record.get("revision")
            if (isinstance(key, str) and re.fullmatch(r"[a-zA-Z0-9_.-]{1,80}", key)
                    and isinstance(text, str) and len(text.encode()) <= 1024
                    and isinstance(stamp, str) and re.fullmatch(r"\d{20}-[a-f0-9]{32}", stamp)
                    and isinstance(record.get("enabled"), bool)):
                if key not in revisions or stamp > revisions[key]["revision"]:
                    revisions[key] = record
    notes = []
    if totals["fixed_waits"] and "prefer-event-waits" not in revisions:
        notes.append("Shared sessions used fixed sleeps. When completion is observable, prefer toolscheme wait-for or process-expect to a fixed delay.")
    for key in sorted(revisions):
        if revisions[key]["enabled"]:
            notes.append(revisions[key]["text"])
    # Stable content means no startup churn when a sync has nothing new to say.
    bounded = []
    for note in notes[:16]:
        if len(("\n".join(bounded + [note])).encode()) <= 8192:
            bounded.append(note)
    value = dict(schema=SCHEMA, hosts=len(hosts), totals=dict(totals), notes=bounded)
    if read_json(state / "learning-snapshot.json") != value:
        atomic(state / "learning-snapshot.json", value)
    atomic(state / "learning-snapshot-head.json", head)
    return value


def init(state, args):
    if (state / "learning-config.json").exists():
        raise RuntimeError("already initialized; existing configuration was preserved")
    host = args.host or uuid.uuid4().hex
    if not re.fullmatch(r"[a-zA-Z0-9_.-]{1,80}", host) or host in (".", ".."):
        raise RuntimeError("invalid host identifier")
    repo = state / "learning-git"
    repo.mkdir(parents=True, exist_ok=True)
    git(repo, "init", "-b", "main")
    git(repo, "config", "user.name", "ToolScheme " + host)
    git(repo, "config", "user.email", host + "@toolscheme.invalid")
    git(repo, "remote", "add", "origin", args.remote)
    atomic(state / "learning-config.json", dict(schema=SCHEMA, host=host, remote=args.remote))
    snapshot(state, repo)
    print("Initialized local learning store. Run toolscheme learn sync, then toolscheme learn schedule.")


def sync(state, config):
    repo = state / "learning-git"
    capture(state, config)
    destination = repo / "records" / config["host"]
    if (repo / "records").is_symlink() or destination.is_symlink():
        raise RuntimeError("record directories must not be symlinks")
    destination.mkdir(parents=True, exist_ok=True)
    pending = sorted((state / "learning-outbox").glob("*.json"))
    for path in pending:
        value = read_json(path)
        if value is None:
            raise RuntimeError("invalid local outbox record")
        target = destination / path.name
        if target.exists() and read_json(target) != value:
            raise RuntimeError("immutable record collision")
        atomic(target, value)
    git(repo, "add", "--", "records")
    if git(repo, "diff", "--cached", "--quiet", check=False).returncode:
        git(repo, "commit", "-m", "Record host learning sessions and steering")
    for path in pending:
        path.unlink()
    snapshot(state, repo)  # durable local progress even if fetch/push fails
    for attempt in range(3):
        git(repo, "fetch", "--no-tags", "origin")
        remote = git(repo, "rev-parse", "--verify", "refs/remotes/origin/main", check=False)
        if remote.returncode == 0:
            merged = git(repo, "merge", "--no-edit", "--allow-unrelated-histories", "origin/main", check=False)
            if merged.returncode:
                git(repo, "merge", "--abort", check=False)
                raise RuntimeError("learning merge conflict; local commits retained for review")
        snapshot(state, repo)
        if git(repo, "rev-parse", "--verify", "HEAD", check=False).returncode:
            print("No learning records yet.")
            return
        pushed = git(repo, "push", "origin", "HEAD:refs/heads/main", check=False)
        if pushed.returncode == 0:
            atomic(state / "learning-sync-status.json", dict(ok=True, at=int(time.time())))
            print("Learning records committed, merged, and pushed.")
            return
    raise RuntimeError("push failed after three fetch/merge attempts; local commits retained")


def schedule(state, args):
    command = [sys.executable, str(Path(__file__).resolve()), "--state", str(state), "sync"]
    if sys.platform == "darwin":
        path = Path.home() / "Library/LaunchAgents/org.toolscheme.learning.plist"
        path.parent.mkdir(parents=True, exist_ok=True)
        content = dict(Label="org.toolscheme.learning", ProgramArguments=command,
                       StartInterval=args.interval, RunAtLoad=True,
                       StandardOutPath=str(state / "learning-scheduler.log"),
                       StandardErrorPath=str(state / "learning-scheduler.log"),
                       EnvironmentVariables={"PATH": os.environ.get("PATH", "/usr/bin:/bin")})
        path.write_bytes(plistlib.dumps(content))
        run(["launchctl", "bootout", "gui/" + str(os.getuid()), str(path)], check=False)
        run(["launchctl", "bootstrap", "gui/" + str(os.getuid()), str(path)])
    elif sys.platform.startswith("linux"):
        directory = Path.home() / ".config/systemd/user"
        directory.mkdir(parents=True, exist_ok=True)
        def quote(arg):
            return '"' + arg.replace("\\", "\\\\").replace('"', '\\"').replace("%", "%%").replace("$", "$$") + '"'
        (directory / "toolscheme-learning.service").write_text(
            "[Unit]\nDescription=Synchronize ToolScheme learnings\n[Service]\nType=oneshot\nExecStart="
            + " ".join(map(quote, command)) + "\n")
        (directory / "toolscheme-learning.timer").write_text(
            "[Unit]\nDescription=Periodically synchronize ToolScheme learnings\n[Timer]\nOnBootSec=60\nOnUnitActiveSec="
            + str(args.interval) + "\n[Install]\nWantedBy=timers.target\n")
        run(["systemctl", "--user", "daemon-reload"])
        run(["systemctl", "--user", "enable", "--now", "toolscheme-learning.timer"])
    else:
        raise RuntimeError("automatic scheduling supports macOS and Linux")
    print("Scheduled learning synchronization every " + str(args.interval) + " seconds.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    default = os.environ.get("TOOLSCHEME_STATE", str(Path(os.environ.get("XDG_STATE_HOME", str(Path.home() / ".local/state"))) / "toolscheme"))
    parser.add_argument("--state", type=Path, default=Path(default))
    sub = parser.add_subparsers(dest="command", required=True)
    setup = sub.add_parser("init")
    setup.add_argument("--remote", required=True)
    setup.add_argument("--host", help="unique host ID; defaults to a random persistent ID")
    sub.add_parser("sync")
    sub.add_parser("status")
    record = sub.add_parser("record", help="publish or revoke a shared steering note")
    record.add_argument("id")
    record.add_argument("text", nargs="?", default="")
    record.add_argument("--disable", action="store_true")
    timer = sub.add_parser("schedule")
    timer.add_argument("--interval", type=int, default=300)
    args = parser.parse_args()
    state = args.state.expanduser().resolve()
    try:
        with locked(state):
            if args.command == "init":
                init(state, args)
                return 0
            config = configuration(state)
            if args.command == "sync":
                sync(state, config)
            elif args.command == "record":
                if not re.fullmatch(r"[a-zA-Z0-9_.-]{1,80}", args.id) or len(args.text.encode()) > 1024 or (not args.disable and not args.text):
                    raise RuntimeError("provide a short ID and 1–1024 bytes of advice")
                enqueue(state, config, dict(kind="steering", id=args.id, text=args.text,
                        enabled=not args.disable, revision=str(time.time_ns()).zfill(20) + "-" + uuid.uuid4().hex))
                print("Learning saved locally; the next sync publishes it.")
            elif args.command == "schedule":
                if args.interval < 60:
                    raise RuntimeError("sync interval must be at least 60 seconds")
                schedule(state, args)
            else:
                print(canonical(dict(config=config, snapshot=read_json(state / "learning-snapshot.json"),
                                     sync=read_json(state / "learning-sync-status.json"))).strip())
        return 0
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        atomic(state / "learning-sync-status.json", dict(ok=False, at=int(time.time()), error=str(error)))
        print("toolscheme learn: " + str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
