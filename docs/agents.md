# Attaching to a coding agent

Toolscheme attaches to an agent through its tool-call hooks. Claude Code and Codex
send the same JSON fields on stdin and accept the same decision JSON on stdout, so
one script serves both; `hooks/observe.sh` locates the project from its own path
rather than from an environment variable, because the two do not agree on what
that variable is called.

## Claude Code

`.claude/settings.json` in this repository already installs it:

```json
{
  "hooks": {
    "PreToolUse":        [{ "matcher": "*", "hooks": [{ "type": "command", "command": "${CLAUDE_PROJECT_DIR}/hooks/observe.sh", "timeout": 5 }] }],
    "PostToolUse":       [{ "matcher": "*", "hooks": [{ "type": "command", "command": "${CLAUDE_PROJECT_DIR}/hooks/observe.sh", "timeout": 5 }] }],
    "PostToolUseFailure":[{ "matcher": "*", "hooks": [{ "type": "command", "command": "${CLAUDE_PROJECT_DIR}/hooks/observe.sh", "timeout": 5 }] }]
  }
}
```

Claude Code may ask you to approve the hook, and a running session needs
restarting to pick it up. Delete the file to turn it off.

## Codex

Codex takes the same handler from TOML, in `~/.codex/config.toml` or a project
config:

```toml
[[hooks.PreToolUse]]
matcher = "*"
[[hooks.PreToolUse.hooks]]
type = "command"
command = "/path/to/toolscheme/hooks/observe.sh"

[[hooks.PostToolUse]]
matcher = "*"
[[hooks.PostToolUse.hooks]]
type = "command"
command = "/path/to/toolscheme/hooks/observe.sh"
```

Codex requires hooks to be trusted before it will run them; it prompts on first
use, and `--dangerously-bypass-hook-trust` skips that for automation that already
vets its hook sources.

Verified against Codex 0.154.0: the hook fires, and a rewrite returned as
`updatedInput` is accepted and executed. Codex normalizes shell calls to
`tool_name: "Bash"` with a plain `{"command": ...}` at hook time, even though its
transcripts record them as `exec` calls wrapping the shell in JavaScript.

Codex is stricter than Claude Code about the decision it accepts: `updatedInput`
must be accompanied by `permissionDecision: "allow"`, a `deny` must carry a
non-empty reason, and `ask`, `continue: false`, `stopReason` and `suppressOutput`
are all rejected. The decision this project emits satisfies both agents.

## Collecting from every session at once

`hooks/observe.sh` finds the project from its own path, not from the session's, so
every session that runs it appends to the same `.toolscheme/observations.jsonl` in
this repository no matter which project the agent is working in. Centralization is
a property of where the hook lives, not of any coordination between sessions.

To collect from everything rather than from this project only, move the handler
from this repository's `.claude/settings.json` to `~/.claude/settings.json`, and
add the TOML block above to `~/.codex/config.toml`. Neither is installed globally
by default.

Three things are worth knowing before doing that:

- Concurrent sessions are safe. Records are clipped below `PIPE_BUF` and appended
  with `O_APPEND`, which is why a 20,000-character command is truncated to about
  2.7 KB rather than being written whole.
- Every tool call in every project then costs about 2.5 ms twice. Across a heavy
  session of several thousand calls that is tens of seconds in total.
- The log will contain commands and working directories from **all** your projects
  in one file. It is gitignored, and it is still one file.

Nothing rotates or prunes it yet.

## What the hook does not do yet

The hook writes; it does not read. Steering a session from what other sessions
have already shown — "you have read this file three times and nothing changed" —
needs the hook to consult the accumulated log on each call, which is a deliberate
design decision with a per-call cost, and is not built.

`additionalContext` is the lever for that: a `PreToolUse` hook may add text to the
model's context without blocking or altering the call. It is worth separating from
rewriting, because the bar is completely different. A rewrite has to reproduce the
command exactly, since the agent cannot see that it happened. Advice changes
nothing about what the tool returns, so it needs no equivalence proof at all —
which makes it the cheaper lever, and the one the measured 541 redundant calls
point at.

## Reading transcripts instead

Nothing has to be installed to analyze history that already exists:

```sh
toolscheme analyze ~/.claude/projects     # Claude Code, nested schema
toolscheme analyze ~/.claude/transcripts  # Claude Code, flat schema
toolscheme analyze ~/.codex/sessions      # Codex rollouts
toolscheme analyze .toolscheme            # this project's own hook log
```

Four schemas, one adapter. What each one can tell you differs:

| | flat | nested | Codex | hook |
|---|---|---|---|---|
| tool and arguments | yes | yes | yes | yes |
| token usage / cache churn | no | yes | per turn | no |
| working directory | no | yes | yes | yes |
| call id joining call to result | no | no | yes | yes |
| duration | no | no | **yes** | **yes** |

Codex records an epoch timestamp and a call id on every record, so durations come
out of its transcripts with no hook installed at all.

Its shell calls arrive wrapped in JavaScript — `tools.exec_command({cmd:"..."})` —
with **unquoted object keys**, so they are JavaScript object literals and not JSON,
and a JSON parser rejects them outright. Both spellings appear in the same corpus
(`cmd:` and `"cmd":`), and one snippet may contain several commands; reading the
value out handles all of it. Matching only the unquoted form silently dropped a
fifth of the shell calls and a third of the `sed -n` invocations, which is the kind
of undercount that looks like a finding rather than a bug.
