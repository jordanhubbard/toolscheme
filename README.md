# Toolscheme

Toolscheme is a small C++17 Scheme interpreter for embedding in coding agents. It
favors native byte strings and immutable random-access array lists over standards
certification, and every tool result is structured, canonical, evaluable data
rather than text to be scraped.

Its purpose was to close a loop: watch what an agent actually calls, measure what
it costs, write a better tool, prove it by replay, and publish it back over MCP.
That was tested against 22,006 recorded calls and 31MB of tool output, and the
second half of it does not hold -- a substitution has no axis left to win on,
because byte-identity makes the byte count equal by construction and a shell call
composes where a tool call does not. The whole substitution stack was removed in
0.5.0. The argument, with the measurements, is in `docs/relevance.md`.

What it is now is an instrument that can also enforce policy: it records what
agents call, it answers questions about that corpus in a language that is already
there, and it refuses a few specific wastes. The refusal is the part with
evidence behind it -- fixed waits of ten seconds or more went from 32 to 2 while
short polling stayed untouched, after an instruction in `AGENTS.md` and hook
advice had both failed to move that number at all.

## Build And Test

```sh
make test        # 1650 checks, warning-clean at -O3
make sanitize    # the same suite under AddressSanitizer + UndefinedBehaviorSanitizer
make fuzz        # deterministic property fuzzer (seeded; reproduces from its seed)
make bench       # benchmarks with enforced performance targets
make loop        # log intake, the hook's decisions, the refusals, MCP
make check       # all of the above
```

## The binary

```sh
toolscheme -e '(+ 20 22)'          # evaluate and print
toolscheme script.scm [args]       # run a script; its value is its output
toolscheme repl                    # interactive
toolscheme mcp                     # stdio MCP server
toolscheme analyze <transcript-dir>  # rank tool-use opportunities from agent logs
```

`--root` sets the sandbox; `--allow-process` and `--allow-program` open specific
child processes; `--telemetry` records every capability call. Nothing reaches the
host that the policy did not permit.

## Installing

Download a relocatable macOS/Linux archive from GitHub Releases, extract it, and
add its `bin` directory to PATH. Keep `bin` and `share` together. Or build from
source with a C++17 compiler:

```sh
make install                    # ~/.local by default
make install PREFIX=/usr/local  # or system-wide
```

A hook that lives in a repository can only watch that repository, so observing
every session means installing outside any checkout. `make install` automatically
adds observation hooks to `~/.claude/settings.json` and `~/.codex/config.toml`,
preserving unrelated settings and backing up changed files beside the originals.
Repeated installs do not add duplicate hooks. This configuration step requires
Python 3.11+. Use `CONFIGURE_HOOKS=0` to install files only, or set
`CLAUDE_CONFIG_DIR` / `CODEX_HOME` to configure alternate agent directories.

## Durable learning across machines

Each host keeps capturing locally. A separate scheduled job commits learning
sessions to Git, merges other hosts' records, pushes, and refreshes a bounded
local startup snapshot. An unavailable remote never delays a tool hook.

```sh
toolscheme learn init --remote git@github.com:YOUR_ACCOUNT/toolscheme-learnings.git
toolscheme learn sync
toolscheme learn schedule
toolscheme learn record bounded-reads 'Read relevant ranges before whole files.'
```

Git stores session summaries and versioned advisory notes, not raw transcripts
or executable generated code. Notes are consumed once per agent session through
the existing hook. Python 3.9+ and Git 2.28+ are required for learning management;
the interpreter itself remains dependency-free. See [the learning guide](docs/learning.md)
for the data contract, offline behavior, revocation, and scheduling.

## Attaching to an agent

`docs/operating.md` is the full account: installing, interposing on one or more
agents, what is collected and where, and how far it reaches (one machine; no
agent-to-agent protocol).

A `PreToolUse` / `PostToolUse` hook records every tool call as it happens: the
command, the directory it ran in, and -- joining the two events by call id -- how
long it took and how many bytes came back. Claude Code and Codex send the same
fields and accept the same decisions, so one script serves both. See
`docs/agents.md` for setup; `.claude/settings.json` installs it for this project,
and `make install` configures observation globally for the current user.

```sh
toolscheme analyze ~/.claude/projects     # Claude Code, nested schema
toolscheme analyze ~/.claude/transcripts  # Claude Code, flat schema
toolscheme analyze ~/.codex/sessions      # Codex rollouts
toolscheme analyze ~/.local/state/toolscheme   # everything the hook has seen
```

The hook **observes by default**: it never denies a call, prints nothing, and
exits 0 whatever happens -- a hook that breaks the session it is measuring is
worse than no measurement. It does nothing at all until `make toolscheme` has been
run. Cost is about 2.5 ms per event.

### Refusing a call

A `PreToolUse` hook may also decline one, and that is the mechanism with evidence
behind it. Advice was measured twice and read past both times; a denial cannot be.

```sh
TOOLSCHEME_REFUSE_SLEEP=1     # decline a fixed wait of ten seconds or more
TOOLSCHEME_DOGFOOD=<paths>    # inside named trees, decline what this replaces
```

Fixed waits at or over the threshold went from 32 to 2 after the first of those,
while short polling -- which the rule deliberately leaves alone -- stayed at 15.
Every refusal names the equivalent to use instead. `docs/agents.md` has the
detail.

A hook may also *rewrite* a call by returning `updatedInput`, and until 0.5.0
this one did, whenever a published tool carried replay evidence of reproducing a
command exactly. That is removed. The gate demanded byte-identity, which is
correct -- an agent cannot see a rewrite, so it must not be able to tell -- but it
makes the byte count equal by construction and leaves only latency, where a fresh
interpreter loses to `fork`. See `docs/relevance.md`.

## What the analysis is for

```sh
toolscheme analyze ~/.claude/projects           # what agents actually call, and what it costs
```

Analysis reports hot tools, shell-AST command shapes, repeat rate, output cost,
and consecutive-call pairs. It runs over the hook's own log as readily as over a
transcript directory, and ad-hoc questions are ordinary Scheme against the same
data -- which is how every figure in `docs/relevance.md` was produced, including
the ones that refuted this project's original charter.

## Embedding

```cpp
#include "toolscheme.hpp"

toolscheme::Interpreter scheme;
auto result = scheme.eval("(eval (read-from-string \"(+ 20 22)\"))");
```

`Value::list_at` is zero-based for C++ callers. Scheme `list-ref` and `string-ref`
are one-based. Strings store a native `size_t` header followed by bytes and a
trailing NUL; `string_data()` exposes that layout and `as_string()` preserves
embedded NUL bytes. Values remain valid after the interpreter that produced them
is destroyed.

## Generated Code

`read`, `read-from-string`, `write`, `write-to-string`, and `eval` make generated
Scheme a first-class interchange format, within one interpreter or between two:

```scheme
(eval (read-from-string (write-to-string '(+ 20 22))))
; => 42
```

Runtime-bound resources never print as unreadable `#<...>` tokens. A handle writes
as an evaluable, runtime-validated reference; evaluating a stale, revoked, or
foreign one returns a structured error.

## Capabilities

Host operations are explicit. Install a `Capability` under a Scheme name and it
becomes the default for its kind, or pass one as the first argument to override:

```scheme
(pwd)                                   ; uses the installed filesystem capability
(pwd workspace-fs '((output data)))     ; or an explicit one
```

Registration groups install only the primitives an installed capability actually
backs. A missing or denied capability returns a structured error; primitives never
reach the host directly. Reference POSIX adapters for Linux and macOS live in
`toolscheme_posix.cpp` behind a policy (sandbox root, executable allowlist,
environment allowlist, output and timeout limits).

## Tools

Text tools accept inline data *or* a tagged source that names files, so the shapes
agents actually reach for work directly:

```scheme
(grep "needle" '(glob "src/**/*.c") '((limit 20)))   ; grep -rn … | head -20
(grep "needle" '(files "a.txt" "b.txt"))
(grep "needle" '("inline" "lines"))                  ; still pure; no host access
```

Matches carry their own path and line numbers relative to their own file. An
unreadable path becomes a record, not an aborted call.

`git` and `http-request` are typed front ends over the generic process API — they
return records, not porcelain text. Full shells, `awk`, compilers, and package
managers stay delegated to a shell or process capability.

## Waiting for an event

```scheme
(wait-for '(exists "build/output"))
(wait-for '(matches "server.log" "Listening on") '((timeout-ms 30000)))
```

Measured across 30 Codex sessions, 17.7% of all tool time -- 9.3 hours of 52.8 --
went to `sleep`, and the four most repeated invocations in the corpus were
identical sleeps of 45 to 60 seconds. `wait-for` returns when the thing actually
happens: against a build that finishes in 3 s, it returned in 3.00 s where the
55 s sleep it replaces would have cost 52 s more.

`process-expect` asks the same question of a process that is not meant to exit:

```scheme
(process-expect job "Listening on" '((timeout-ms 30000)))
```

It blocks on the pipe rather than polling, and a process that exits without ever
printing the pattern ends the wait instead of serving out the deadline. This is the
other half of the measured waste: 1,318 calls in the corpus wrote an empty string
to an interactive session purely to see whether it had finished yet.

## Analytical queries

Optional, and off unless built for. The collector stays an append-only file --
capture must not depend on anything being reachable -- and DuckDB reads that file
for analysis:

```sh
make vendor-duckdb && make toolscheme
```

```scheme
(sql-query "SELECT tool, count(*) AS calls FROM read_json_auto('...', ignore_errors=true)
            WHERE event='pre' GROUP BY tool ORDER BY calls DESC")
```

The same question over a 41,000-record log: **23.1 s** walking the JSON in Scheme,
**0.11 s** in SQL. Queries are confined to the capability root like every other
path in the API.

## Stable output

Results are stable by default: host metadata that churns between otherwise
identical calls — modification times, inode numbers, elapsed milliseconds — is
omitted unless requested. Repeating a call yields byte-identical output, which is
what keeps an agent's prompt cache warm.

```scheme
(stat "a.txt")                           ; no volatile metadata
(stat "a.txt" '((volatile #t)))          ; include it
(stat "a.txt" '((fields (path kind size))))
```

Identifiers a caller must act on, such as a job's `pid`, are never stripped.

`docs/api.md` is the full contract: result shape, output options, capability
policy, and the stability rule. `docs/roadmap/active-work.md` tracks the tool
surface, `docs/roadmap/self-improvement.md` the loop, and
`tests/roadmap-test-map.md` maps every roadmap item to its test.
