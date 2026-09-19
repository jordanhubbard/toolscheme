# Durable learning with Git

The backing store is a separate Git repository. Each host captures locally, commits immutable learning records, fetches and merges other hosts' records, and pushes periodically. Git and network availability never enter the tool-call path. Startup reads the last local snapshot, including when offline. A fresh host gets the shared state on its first successful sync.

## Install on each machine

Use the release archive for your OS/architecture, or build with a C++17 compiler and `make install`. Add the installation's `bin` directory to PATH. The base interpreter needs no third-party libraries. Learning management additionally needs Python 3.9+ and Git 2.28+.

Create a private, dedicated Git repository and give each host normal Git read/write access. Then:

```sh
toolscheme learn init --remote git@github.com:YOUR_ACCOUNT/toolscheme-learnings.git
toolscheme learn sync
toolscheme learn schedule                 # every 300 seconds
toolscheme learn status
```

`init` generates a persistent random host ID. Do not copy an initialized state directory to another host; initialize each separately. The optional `--host NAME` is for explicit, unique host IDs. All commands accept `--state DIRECTORY` before the subcommand. Otherwise they use `TOOLSCHEME_STATE`, then `$XDG_STATE_HOME/toolscheme`, then `~/.local/state/toolscheme`.

`schedule` installs a user LaunchAgent on macOS or a systemd user timer on Linux. It starts at login/user-manager startup and runs independently of agents. Git credentials must work without prompting. A Linux user manager needs lingering if synchronization should continue after logout. For hosts without either scheduler, invoke `toolscheme learn sync` from an existing scheduler; failures return nonzero and leave local state available. No scheduler is installed by `make install`.

Register `hooks/observe.sh` as described in `operating.md`. A `SessionStart` registration delivers shared notes immediately; if the agent only provides `PreToolUse`, the first tool hook delivers them instead. Notes are delivered once per session. Both paths use only the snapshot. An already-running MCP process sees new learnings on its next restart; each new CLI invocation reads them at startup as `learning-snapshot` and `learning-notes`.

## What is durable

The hook's raw `observations.jsonl` remains local. Sync reads complete lines in batches of at most 4 MiB and checkpoints its byte offset. Each batch produces immutable records per session with call counts, result bytes, failures, and fixed-sleep observations. Session identifiers are hashed. Raw commands, inputs, responses, paths, and raw session IDs are not copied into Git. This preserves measurable session learnings without turning the shared repository into a transcript archive.

The session summaries currently derive one conservative shared note: prefer observable completion to fixed sleeps when fixed sleeps have been measured. Record other conclusions explicitly:

```sh
toolscheme learn record bounded-reads 'Read the relevant ranges before requesting whole files.'
toolscheme learn sync
toolscheme learn record bounded-reads --disable
```

A record is durable in the local outbox immediately. The next sync puts it in a local Git commit before trying the network. Shared notes are plain advisory text; they cannot install capabilities, execute Scheme, or automatically publish generated tools. Do not put credentials into manually written notes. Existing replay/publication gates still govern executable tools.

## Storage and convergence

- `learning-git/records/HOST/SHA256.json`: canonical, immutable, schema-versioned records. Separate files let Git merge concurrent hosts without both rewriting a shared log or index.
- `learning-outbox/`: crash-safe local records waiting for a commit.
- `learning-cursor.json`: local byte checkpoint. Checkpointing follows durable outbox writes; replaying a batch produces identical filenames.
- `learning-capture.json`: a prepared batch and its next checkpoint. Recovery finishes this exact batch even if capture continued while the worker was down.
- `learning-snapshot.json`: an atomically replaced, bounded read model. Startup reads at most 64 KiB and consumes at most 16 notes / 8 KiB.
- `learning-sync-status.json`: last sync status, including offline failure.

No-op syncs create no commits, skip rebuilding an unchanged Git snapshot, and leave its file untouched. For new commits, Git's changed-path list drives incremental aggregation; only newly added record files need reading. Modified/deleted records or a missing local index trigger a full rebuild from Git. The index is disposable and never authoritative. Sync fetches only the configured remote and no tags, merges without rebasing or force-pushing, and retries a rejected push up to three times. Each Git process has a 45-second deadline. A process lock prevents overlapping local sync/record operations and is released automatically on process exit.

Steering revisions are immutable events. For an ID, the greatest `(timestamp-in-nanoseconds, random-ID)` revision wins, including disabled revisions. Host clocks should be synchronized; a host with a future clock can otherwise win until corrected. To roll back advice, publish a new revision or disable its ID. `prefer-event-waits` controls the automatically derived wait advice too. Genuine Git conflicts stop sync and preserve local commits for manual review. Existing startup state remains usable.

The snapshot is a derived cache and can be rebuilt from Git with `sync`. The cursor, outbox, and local Git commits are host state: preserve them together when restoring a host backup. Git history is intentionally retained; no force push or history pruning is automatic. Large deployments should monitor repository growth and archive old histories deliberately.

## Scheduling lifecycle

On macOS, inspect `~/Library/LaunchAgents/org.toolscheme.learning.plist`; output goes to `learning-scheduler.log` in the state directory. Stop with `launchctl bootout gui/$(id -u) ~/Library/LaunchAgents/org.toolscheme.learning.plist` and remove that plist to prevent restarting at login.

On Linux, inspect with `systemctl --user status toolscheme-learning.timer` and `journalctl --user -u toolscheme-learning.service`. Stop with `systemctl --user disable --now toolscheme-learning.timer`. Remove the corresponding service and timer files from `~/.config/systemd/user` before uninstalling.

Upgrading in place preserves state. Move an installation only after stopping its scheduler, then run `schedule` from the new installation. Uninstalling the binary never deletes learning state or the remote repository.
