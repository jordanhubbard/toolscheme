# Self-improvement loop

The purpose of the project: watch what a coding agent actually calls, measure what
it costs, write a better tool, prove it, and hand it back — so the tool collection
adapts to how agents work instead of inheriting Unix's 1970s ergonomics.

```
Claude Code / Codex transcripts ─┐
   pasted session text ──────────┼─→ agent-log-events ─→ opportunity report
   toolscheme self-telemetry ────┘        (Scheme)            (Scheme)
                                                                  │
                                              synthesize ←────────┘
                                          (HTTP capability)
                                                  │  Scheme source + metadata
                                                  ↓
                                        differential replay
                                     legacy path │ new tool     ← gate
                                                  │
                                                  ↓  only if equivalent AND cheaper
                                      lib/tools/<name>.scm  ─→ MCP  ─→ agent
```

## Status

| Stage | Where | State |
|---|---|---|
| Intake | `lib/agentlog.scm` | Both discovered transcript schemas, from a path or pasted text; tool names normalized across their spellings |
| Analysis | `lib/analysis.scm` | 158 transcripts / 14,129 events in ~3s |
| Synthesis | `lib/synthesis.scm` | Request, cache breakpoint, and every response branch verified offline; **the live call is unverified — no API key in the development environment** |
| Gate | `lib/replay.scm` | Proven in both directions by `make loop` |
| Publication | `define-tool`, `lib/tools/*.scm`, MCP | `search-read` published and callable over MCP |

## What the corpus said

158 transcripts, 14,129 events, 7,097 tool calls, 2,635 shell invocations,
127 MB of tool results.

Top opportunities after ranking by recoverable waste:

| Pattern | Occurrences | Kind |
|---|---:|---|
| `read -> read` | 1203 | fusion |
| `edit -> edit` | 405 | fusion |
| `read -> edit` | 241 | fusion |
| `grep -> read` | 192 | fusion |
| `head -20` | 274 | command shape |
| `curl -s` | 264 | command shape |
| `grep -n` | 259 | command shape |

Also measured: **541 redundant calls** across 274 repeated invocations — one file
was read 63 times in a single corpus.

Cache accounting needs the nested schema, which is the only one carrying per-turn
token usage. On a single long session it reports 159.4M cache-read tokens against
8.2M cache-created, with `bash` responsible for 5.85M of the creation — that is
the churn figure the loop exists to drive down. On a transcript format that
records no usage, the report says so rather than printing zeros that look like a
measurement.

Two ranking corrections were needed to get here, and both matter more than they
look:

- **Parse shell properly.** A regex sweep reported `e`, `if`, and `out.field` as
  top commands, because it read heredoc bodies and quoted C++ as shell. `shell-parse`
  is a real tokenizer; `for`/`do`/`done` are grammar, not programs.
- **Rank by recoverable waste, not frequency.** Raw frequency nominates `echo`
  (598 calls), the least worth replacing: nothing reads its output. An opportunity
  is only real if a structured, bounded, stable tool would return something the
  agent consumes.

## The gate

`make loop` replays two candidates against the shell commands they claim to
replace. Both are stable and both are cheaper. Only one is published:

| Candidate | Equivalent | Stable | Legacy bytes | Candidate bytes | Published |
|---|---|---|---:|---:|---|
| `search-read` | yes | yes | 22,775 | 5,009 | **yes** |
| `search-read-lossy` | no | yes | 22,775 | 4,521 | no |

The lossy candidate silently drops every second match. It is *cheaper than the
real tool* and perfectly stable, and it is refused anyway, with the disagreeing
line as evidence. Winning on cost is never a substitute for agreeing on the
answer: a faster tool that answers differently is not a replacement, it is a bug
with better latency.

Comparison is by rendering, not substring: the candidate projects its structured
result into the legacy tool's shape and equivalence is judged there. The legacy
side is charged for the follow-up reads the recorded pattern performed, because a
fused tool that replaces `grep -> read` should not be compared against only the
grep half.

## Open

- **The live synthesis call is unverified.** Everything around it is covered, but
  no tool in `lib/tools/` has yet been written by a model. `make synthesize` runs
  the loop end to end once `ANTHROPIC_API_KEY` is set.
- **Replay cases are not yet derived from transcripts.** The gate needs recorded
  invocations; the analyzer reports patterns but does not yet carry the concrete
  calls that produced them, so `synthesize-live` refuses rather than assuming.
- **Prompt injection.** Transcripts are untrusted input, fed to a model whose
  output becomes executable Scheme. The capability sandbox bounds the blast
  radius, and a synthesized tool should be published without process or network
  capabilities unless its pattern requires them. This needs an explicit policy
  decision before the loop runs unattended.
- **Replay proves equivalence only on recorded inputs.** The legacy path stays
  callable and publication is a file, so reverting is a deletion.
- **Platform breadth.** `platform-facts` lands here so a generated tool can branch
  on host facts; Windows and WSL adapters come after the loop is proven.
