# Attaching to a coding agent

## Install it first

A hook that lives in a repository can only ever watch that repository, because the
agent configuration has to name an absolute path to it. To observe every session,
install the binary, the library and the hook somewhere outside any checkout:

```sh
make install                    # ~/.local by default
make install PREFIX=/usr/local  # or system-wide
```

That puts `toolscheme` in `$PREFIX/bin` and the rest in
`$PREFIX/share/toolscheme`, and configures observation hooks in your Claude and
Codex user settings. Observations go to
`$XDG_STATE_HOME/toolscheme` (`~/.local/state/toolscheme`), which is also the
sandbox root the hook runs under -- so it watches every project and can write to
none of them.

Nothing is written to your agent configuration for you; the snippets are printed
for you to review and paste.

There is no daemon. Each hook invocation is a short-lived process, which is why the
cost is about 2.5 ms rather than nothing; a resident service would remove that
start-up, and would matter for rewriting and for any future steering that has to
consult accumulated state, but it is not needed to collect.

Toolscheme attaches to an agent through its tool-call hooks. Claude Code and Codex
send the same JSON fields on stdin and accept the same decision JSON on stdout, so
one script serves both; `hooks/observe.sh` locates the project from its own path
rather than from an environment variable, because the two do not agree on what
that variable is called.

## Claude Code

`~/.claude/settings.json` for every session, or `.claude/settings.json` in one
repository for that project alone. This repository carries the latter, pointing at
its own checkout:

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

Codex requires a hook to be trusted before it will run it, and **an untrusted hook
is skipped silently** — `codex exec` completes normally, says nothing about hooks,
and records nothing. Verified: the same command records two events with
`--dangerously-bypass-hook-trust` and none without it. Trust once from an
interactive session; the bypass flag does not persist it.

Codex also reads its configuration at startup, so a session already running when
the hook is added will never fire it, however long it lives.

One more difference worth knowing when comparing the two: Codex invokes the hook
once per event. Claude Code invokes it twice.

Verified against Codex 0.154.0: the hook fires, and a rewrite returned as
`updatedInput` is accepted and executed. Codex normalizes shell calls to
`tool_name: "Bash"` with a plain `{"command": ...}` at hook time, even though its
transcripts record them as `exec` calls wrapping the shell in JavaScript.

Codex is stricter than Claude Code about the decision it accepts: `updatedInput`
must be accompanied by `permissionDecision: "allow"`, a `deny` must carry a
non-empty reason, and `ask`, `continue: false`, `stopReason` and `suppressOutput`
are all rejected. The decision this project emits satisfies both agents.

## Collecting from every session at once

`hooks/observe.sh` finds its installation from its own path, not from the session's,
so every session that runs it appends to the same `observations.jsonl` in the state
directory no matter which project the agent is working in. Centralization is a
property of where the hook lives, not of any coordination between sessions.

To collect from every project, run `make install`. It updates
`~/.claude/settings.json` and `~/.codex/config.toml` automatically, preserving
unrelated settings and backing up changed files. Repeated installs do not duplicate
the hooks. Configuration requires Python 3.11+; use `CONFIGURE_HOOKS=0` to skip it.
`CLAUDE_CONFIG_DIR` and `CODEX_HOME` override the agent configuration directories.

Three things are worth knowing before doing that:

- Concurrent sessions are safe. Records are clipped below `PIPE_BUF` and appended
  with `O_APPEND`, which is why a 20,000-character command is truncated to about
  2.7 KB rather than being written whole.
- Every tool call in every project then costs about 2.5 ms twice. Across a heavy
  session of several thousand calls that is tens of seconds in total.
- The log will contain commands and working directories from **all** your projects
  in one file, outside any of them.

Nothing rotates or prunes it yet.

## Telling the agents apart

Both agents spell a shell call `Bash`, so the tool name cannot distinguish them and
a merged corpus would silently average two different sets of habits. Each record
therefore carries an `agent`, keyed on `turn_id` -- Codex's own documented
extension to the hook payload, which Claude Code does not send.

This is not a cosmetic distinction. Across their transcript corpora the top command
shape is `sed -n` for Codex and `head -20` for Claude Code: genuinely different
ways of reading a file, and exactly the kind of difference that disappears into an
average.

## The log is a stream, not a ledger

An agent may invoke a hook more than once for the same event. Claude Code calls
this one **twice per tool call**, a few milliseconds apart with an identical
`tool_use_id`, from a single registration. Taking the log at face value therefore
doubles every count in every report — tool counts, shell calls, repeat rates,
latency pairing, all of it, silently and plausibly.

So an event is identified by session, call id and which end of the call it is, and
the reader keeps the first of each. Records from other schemas keep their position
as their identity and are never dropped. Order survives deduplication, because
consecutive-call analysis depends on it.

Treat the file as an append-only stream that may repeat itself, not as a list of
distinct events. That is also what makes it safe for many sessions to append to one
file with no coordination.

## The observer must not observe itself

A `PostToolUse` event carries the tool input that actually ran, which after a
rewrite is toolscheme's own invocation rather than the command the agent asked for.
Left unmarked, those records feed straight back into the shape rankings that decide
what to replace next, and the analyzer ends up measuring its own output and
reporting it as demand.

Records whose command invokes `run-tool.scm` are therefore marked `rewritten`, and
the analyzer drops their command while still counting the call, its duration and
its bytes -- the call really happened, but its shape was not a choice anyone made.
This showed up by reading the log after the first live Codex rewrite, where the
`pre` record held `grep -n define-tool ...` and the `post` record held the
substitution.

## Telling the agent, and where

Measured across a 2x2 of three trials each: left to itself the agent polls, in
every trial. A one-line note in the agent's own instructions removes the behaviour
entirely; the per-call hook advice corrects it only after the first occurrence,
because it cannot fire until the agent has already written the loop.

| | adopted the tool | polled in a loop |
|---|---|---|
| neither | 0/3 | 3/3 |
| hook only | 3/3 | 3/3 |
| note only | 3/3 | **0/3** |

So put the note in `~/.codex/AGENTS.md` or `~/.claude/CLAUDE.md` first. The hook is
for where those cannot be edited.

Once the note is in the agent's own instructions, a `SessionStart` hook carrying
the same text is not a second safeguard, it is the same sentence charged to the
context twice. The configuration this repository recommends and runs:

| | instructions | hooks |
|---|---|---|
| Claude Code | `~/.claude/CLAUDE.md` | `PreToolUse`, `PostToolUse`, `PostToolUseFailure` |
| Codex | `~/.codex/AGENTS.md` | `PreToolUse` |

Claude Code keeps `PostToolUse` because its transcripts record no per-call timing
and pairing the two events is the only way to get a duration. Codex does not need
it: its rollouts already carry a timestamp and a call id on every record, so a
second hook would be writing down what has already been written.

### Checking that it landed

Claude Code does not record its loaded instructions in the transcript, so whether
`CLAUDE.md` was read cannot be inspected directly. It can be judged by behaviour,
which is what the 2x2 measured:

```sh
make adoption
```

| verdict | meaning |
|---|---|
| `instruction-working` | used the tool, never polled, was never advised -- the note did its job |
| `corrected-after-polling` | polled, was advised, then used the tool -- the hook is carrying it, the note did not load |
| `polled-uncorrected` | neither channel reached this session |
| `waited-without-polling` | waited some other way; no complaint, no evidence either |
| `nothing-to-judge` | the session never waited for anything |

Instructions are read at session start, so a session already running when the note
was added will show `corrected-after-polling` at best. That is the expected result,
not a failure.

## Advice

`TOOLSCHEME_STEER=1` lets the hook add a note to the model's context without
touching the call. Two things trigger it, both from the measured corpus:

- **A fixed wait of ten seconds or more.** 9.3 hours of 52.8 went to `sleep`, in
  identical 45-to-60 second increments. The note names `wait-for` and
  `process-expect`. Said once per session -- repeating it every call would cost
  more context than the advice saves.
- **An invocation already made in this session.** 541 redundant calls for Claude
  Code, 2,149 for Codex, one file read sixty-three times. Said every time, because
  it names a specific call and stays true.

Verified reaching a live Codex session: asked to quote the guidance it received,
the model reported "condition-based waits can finish as soon as a condition is
met".

The bar for advice is different in kind from the bar for a rewrite. A rewrite has
to be proven byte-identical because the agent cannot see that it happened. Advice
changes nothing about what the call returns, so it cannot be wrong about the
answer -- only about whether it was worth the context it cost. Which makes staying
quiet the property worth getting right, and most of what is tested.

Advice alone carries no `permissionDecision`: Codex rejects `allow` unless a
rewrite accompanies it, and adding one would turn a note into a permission grant
the hook never meant to make.

### Turning it on for every session

An environment variable only reaches a hook if the agent inherited it, and whether
it did depends on whether the session began from a shell, a desktop launcher, or
another agent. So a setting is read from the environment when it is there and from
a configuration file when it is not:

```
# ~/.config/toolscheme/config   (the state directory is still read, for older installs)
TOOLSCHEME_STEER=1
```

`NAME=value` per line, `#` for comments, `0` or `false` to turn something off. The
environment wins where both say something, so a single session can differ from the
default without editing anything. `TOOLSCHEME_REDIRECT` is read the same way.

Verified against a live Codex session with `TOOLSCHEME_STEER` explicitly unset in
its environment: the advice still arrived.

### Telling the agent, and where

Measured across a 2x2 of three trials each: left to itself the agent polls, in
every trial. A one-line note in the agent's own instructions removes the behaviour
entirely; the per-call hook advice corrects it only after the first occurrence,
because it cannot fire until the agent has already written the loop.

| | adopted the tool | polled in a loop |
|---|---|---|
| neither | 0/3 | 3/3 |
| hook only | 3/3 | 3/3 |
| note only | 3/3 | **0/3** |

So put the note in `~/.codex/AGENTS.md` or `~/.claude/CLAUDE.md` first. The hook is
for where those cannot be edited.

Once the note is in the agent's own instructions, a `SessionStart` hook carrying
the same text is not a second safeguard, it is the same sentence charged to the
context twice. The configuration this repository recommends and runs:

| | instructions | hooks |
|---|---|---|
| Claude Code | `~/.claude/CLAUDE.md` | `PreToolUse`, `PostToolUse`, `PostToolUseFailure` |
| Codex | `~/.codex/AGENTS.md` | `PreToolUse` |

Claude Code keeps `PostToolUse` because its transcripts record no per-call timing
and pairing the two events is the only way to get a duration. Codex does not need
it: its rollouts already carry a timestamp and a call id on every record, so a
second hook would be writing down what has already been written.

### Checking that it landed

Claude Code does not record its loaded instructions in the transcript, so whether
`CLAUDE.md` was read cannot be inspected directly. It can be judged by behaviour,
which is what the 2x2 measured:

```sh
make adoption
```

| verdict | meaning |
|---|---|
| `instruction-working` | used the tool, never polled, was never advised -- the note did its job |
| `corrected-after-polling` | polled, was advised, then used the tool -- the hook is carrying it, the note did not load |
| `polled-uncorrected` | neither channel reached this session |
| `waited-without-polling` | waited some other way; no complaint, no evidence either |
| `nothing-to-judge` | the session never waited for anything |

Instructions are read at session start, so a session already running when the note
was added will show `corrected-after-polling` at best. That is the expected result,
not a failure.

## Advice has to be actionable

Naming a tool is not enough. The first version of the sleep note said toolscheme
"has `wait-for`" without saying how to reach it, and reaching it turned out not to
work: an installed binary could not find its own library, because `locate_library`
looked beside the executable and in `../lib` but not in `../share/toolscheme/lib`,
and `wait-for` rejected every path it was given because it used the policy root raw
where `.` is not an absolute path and fails every containment check. Both were
invisible until the tool was invoked the way an agent would invoke it, with no
flags, from an unrelated directory.

The note now gives a command that runs:

```
toolscheme -e '(wait-for (quote (exists "some/path")))'
```

## Continuing without waiting

Off unless switched on, in the state directory's `config`:

```
TOOLSCHEME_CONTINUE=1        # decline the stop when the agent named its next step
TOOLSCHEME_CONTINUE_MAX=3    # consecutive continuations before it stops anyway
```

Claude Code only; Codex has no `Stop` hook to decline.

Measured across 30 Codex sessions: 34.5 hours idle waiting for a person, 10.1 of
them in gaps short enough to be worth recovering, against 2.7 hours asleep on a
timer. But the obvious use of that is not what the data supports. Of 141 such
stalls, **2** ended with a question or an offer to proceed; the other 139 ended
with a completion summary that named the next step and stopped anyway. So this does
not approve decisions on anyone's behalf. It declines to stop when the agent has
already said what it would do next.

It refuses in every other case:

| the agent's last message | result |
|---|---|
| names a next step, asks nothing | continues |
| asks a question | stops |
| names a next step *and* asks a question | **stops** -- the question wins |
| a plain sign-off with no next step | stops |
| empty | stops |
| already continued `TOOLSCHEME_CONTINUE_MAX` times | stops |

The third row is the one that matters. Answering a question on the user's behalf is
the failure that would make this indefensible, so a question overrides every other
signal.

This is the most dangerous feature here, because an agent that does not stop has no
natural place left to check its own work. The cap exists so a loop ends by
arithmetic rather than by someone noticing.

## Keeping the installation current

The hook runs the *installed* copy, not the checkout. After changing anything under
`lib/` or `hooks/`, run `make install` again or the hook will keep running the old
code -- silently, because a hook that cannot find a feature simply does not use it.
Steering appeared to do nothing on its first live test for exactly this reason.

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
toolscheme analyze ~/.local/state/toolscheme   # everything the hook has seen
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
