# Operating toolscheme

What it is, how to run it, what it can and cannot reach.

## In one paragraph

Toolscheme watches the tool calls a coding agent makes, measures what they cost,
and can act on what it finds — by telling the agent something useful, or by
substituting a tool that has been proven equivalent. It attaches at the agent's
**tool-call boundary**, through the hook mechanism the agent already has. There is
no agent-to-agent protocol involved; see [Reach](#reach) for exactly how far it
sees.

## Install

```sh
git clone https://github.com/jordanhubbard/toolscheme && cd toolscheme
make                         # builds `toolscheme` and the test binary
make check                   # the full gate, if you want it
make install                 # to ~/.local by default
make install PREFIX=/usr/local
```

`make install` prints the agent configuration to add. It writes none of it for you.

Optionally, for fast ad-hoc queries over the collected log:

```sh
make vendor-duckdb && make toolscheme && make install
```

Nothing requires DuckDB. Without it the `sql-query` primitive does not exist, which
is how every capability-backed primitive here behaves when nothing backs it.

## Run

```sh
toolscheme -e '(+ 20 22)'              # evaluate and print
toolscheme script.scm [args]           # run a script; its value is its output
toolscheme repl                        # interactive
toolscheme mcp                         # stdio MCP server: published tools + toolscheme_eval
toolscheme analyze <dir>               # rank tool-use opportunities from logs
```

Policy flags bound what the interpreter may reach: `--root` sets the filesystem
sandbox, `--allow-process` and `--allow-program` open specific child processes,
`--read-only` refuses mutation. A primitive whose capability is absent does not
exist rather than failing at call time.

## Interposing on agents

Both supported agents call an external program before and after each tool call,
passing the same JSON on stdin and accepting the same JSON decision on stdout. One
script, `hooks/observe.sh`, serves both; it locates its own installation from its
path, so a single copy handles every session on the machine.

### Claude Code

`~/.claude/settings.json`:

```json
{ "hooks": {
    "PreToolUse":        [{ "matcher": "*", "hooks": [{ "type": "command", "command": "/home/you/.local/share/toolscheme/hooks/observe.sh", "timeout": 5 }] }],
    "PostToolUse":       [{ "matcher": "*", "hooks": [{ "type": "command", "command": "/home/you/.local/share/toolscheme/hooks/observe.sh", "timeout": 5 }] }],
    "PostToolUseFailure":[{ "matcher": "*", "hooks": [{ "type": "command", "command": "/home/you/.local/share/toolscheme/hooks/observe.sh", "timeout": 5 }] }] } }
```

`PostToolUse` earns its place here because Claude Code transcripts record no
per-call timing; pairing the two events is the only way to get a duration.

### Codex

`~/.codex/config.toml`:

```toml
[[hooks.PreToolUse]]
matcher = "*"
[[hooks.PreToolUse.hooks]]
type = "command"
command = "/home/you/.local/share/toolscheme/hooks/observe.sh"
```

One hook, not three. Codex rollouts already carry a timestamp and a call id on
every record, so a `PostToolUse` hook would be writing down what is already
written, and each hook has to be trusted by hand.

**Codex skips untrusted hooks silently** — `codex exec` completes normally, says
nothing, and records nothing. Trust it once from an interactive session. Codex also
reads its configuration at startup, so a session already running when the hook is
added will never fire it.

### Adding a third agent

Two things are needed, and only the first is required:

1. **A hook registration** in that agent's configuration, pointing at
   `observe.sh`. If the agent sends `tool_name`, `tool_input`, `cwd`,
   `session_id` and `tool_use_id`, nothing else is needed — both supported agents
   happen to agree on those names.
2. **A log adapter**, only if you also want to analyse that agent's own
   transcripts offline. `lib/agentlog.scm` holds four: two Claude Code formats,
   Codex rollouts, and toolscheme's own hook records. Dispatch is by a field that
   only one schema has.

## Reach

This is the part worth being precise about.

**It does not use A2A, or any agent-to-agent protocol.** It does not speak to
agents at all. It interposes *beneath* one — between the agent and the tool it is
about to run — by being the program that agent already invokes at that boundary.

**Capture is local to each machine.** The hook is a local process; it appends to a local
file; the analysis reads that file. Two agents on one machine share one log, which
is why a Claude Code session and a Codex session appear side by side in the same
report. Hosts configured with the same learning Git remote share session summaries
and steering notes through periodic synchronization; raw observations remain local.

**MCP is the other direction.** `toolscheme mcp` serves published tools *to* an
agent; it is not how observations are collected.

See [Durable learning with Git](learning.md) for configuration. Capture stays a local
append-only file, and a separate job commits summaries to Git. The reason
capture must stay local and dependency-free is that the hook runs on every single
tool call, and one that needs a network or a database turns an outage into a
failure in every session on the machine.

## What it collects, and where

`$XDG_STATE_HOME/toolscheme` (`~/.local/state/toolscheme`), which is also the
sandbox root the hook runs under — so it watches every project and can write to
none of them.

| file | what |
|---|---|
| `observations.jsonl` | one record per hook invocation, append-only |
| `sessions/<id>.keys` | per-session invocation keys, for repeat detection |
| `config` | settings, read when the environment does not answer |

A record holds the tool, the command, the working directory, the session and call
ids, a timestamp, and the result size. Commands are clipped, and the clip respects
UTF-8 character boundaries.

Two things to know before turning it on for every project:

- The log grows about 22 MB a day under steady use, and **nothing rotates it**.
- It will contain commands and working directories from **every project** either
  agent touches, in one file outside all of them.

## Analysis

```sh
toolscheme analyze ~/.local/state/toolscheme   # everything the hook has seen
toolscheme analyze ~/.claude/projects          # Claude Code transcripts
toolscheme analyze ~/.codex/sessions           # Codex rollouts
make adoption                                  # did the instruction land?
```

The report ranks tools, shell command shapes, repeated invocations, fusion
candidates and — where the source records timings — where the time went.

With DuckDB built, ad-hoc questions are much faster than the full report:

```scheme
(sql-query "SELECT tool, count(*) AS n
            FROM read_json_auto('/home/you/.local/state/toolscheme/observations.jsonl',
                                ignore_errors=true)
            WHERE event = 'pre' GROUP BY tool ORDER BY n DESC")
```

0.11 s against 23 s for the same question walked in Scheme. The full `analyze` is
not built on it, because measurement showed the cost there is shell *parsing*, not
reading, and that stays in Scheme either way.

## Steering

Off unless asked for, in `~/.local/state/toolscheme/config`:

```
TOOLSCHEME_STEER=1      # advise, per call
TOOLSCHEME_REDIRECT=1   # substitute a proven tool, per call
```

Advice adds text to the model's context and changes nothing about the call.
Redirection replaces the call, and is allowed only for a command shape a published
tool has been proven to reproduce byte for byte, exit status included, *and* to
beat on time. Nothing currently claims that, so nothing is rewritten.

Measured, a one-line note in the agent's own instructions (`CLAUDE.md`,
`AGENTS.md`) beats the per-call hook: it prevents the behaviour rather than
correcting it after the fact. Put the note there first; the hook is for where
instructions cannot be edited.

## Security posture

- Every path resolves inside the capability root; traversal and symlink escapes are
  refused before any host call.
- Child processes are refused unless explicitly allowed, by name.
- SQL queries are confined to the same root, and the adapter verifies the
  confinement took effect rather than assuming it.
- The hook never blocks a call, prints nothing unless it has a decision, and exits
  0 whatever happens. A hook that breaks the session it measures is worse than no
  measurement.
- Nothing is installed into your agent configuration without you pasting it.
