# Toolscheme

Toolscheme is a small C++17 Scheme interpreter for embedding in coding agents. It
favors native byte strings and immutable random-access array lists over standards
certification, and every tool result is structured, canonical, evaluable data
rather than text to be scraped.

Its purpose is to close a loop: watch what an agent actually calls, measure what it
costs, write a better tool, prove it by replay, and publish it back over MCP. See
`docs/roadmap/self-improvement.md`.

## Build And Test

```sh
make test        # 1650 checks, warning-clean at -O3
make sanitize    # the same suite under AddressSanitizer + UndefinedBehaviorSanitizer
make fuzz        # deterministic property fuzzer (seeded; reproduces from its seed)
make bench       # benchmarks with enforced performance targets
make loop        # log intake, tool library, MCP, synthesis, and the publication gate
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

## Observing an agent live

A `PreToolUse` / `PostToolUse` hook records every tool call as it happens: the
command, the directory it ran in, and -- joining the two events by call id -- how
long it took and how many bytes came back. Transcripts give none of that reliably;
one schema records no working directory at all, and neither records timings.

```sh
toolscheme analyze .toolscheme    # analyze what this project's agent actually did
```

`.claude/settings.json` installs it for this project. It **observes only**: it
never denies a call, never rewrites one, prints nothing, and exits 0 whatever
happens -- a hook that breaks the session it is measuring is worse than no
measurement. It does nothing at all until `make toolscheme` has been run. Delete
`.claude/settings.json` to turn it off.

Cost is about 2.5 ms per event, which is less than a single `fork`+`exec`.

## The loop

```sh
toolscheme analyze ~/.claude/transcripts        # what is worth replacing, and why
make loop                                       # prove a candidate before publishing
make synthesize                                 # let a model write the next one
```

Analysis of 158 transcripts — 14,129 events, 7,097 tool calls — runs in about three
seconds and reports hot tools, shell-AST command shapes, repeat rate, output cost,
and consecutive-call pairs as fusion candidates.

A candidate is published only if it agrees with the tool it replaces on every
replayed case *and* wins on bytes, latency, or stability. A deliberately lossy
candidate that is both stabler and cheaper is refused, with the disagreeing line as
evidence — winning on cost never substitutes for agreeing on the answer.

`make synthesize` needs a credential: `NVIDIA_INFERENCE_API_KEY` for the NVIDIA
inference gateway, or `ANTHROPIC_API_KEY` to talk to Anthropic directly. The gate
only ever replays commands whose every program reads and reports — a corpus is full
of commands that must never be re-run.

Published tools live in `lib/tools/*.scm` with a provenance header naming the
pattern that motivated them and what the replay measured. Publishing is writing a
file; reverting is deleting one.

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
