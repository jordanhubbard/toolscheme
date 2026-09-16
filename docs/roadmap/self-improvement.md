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
                                                  ↓  only if equivalent AND better
                                      lib/tools/<name>.scm  ─→ MCP  ─→ agent
```

## Status

| Stage | Where | State |
|---|---|---|
| Intake | `lib/agentlog.scm` | Both discovered transcript schemas, from a path or pasted text; tool names normalized, working directory carried |
| Analysis | `lib/analysis.scm` | 158 transcripts / 14,129 events in ~3s |
| Synthesis | `lib/synthesis.scm` | Live against the NVIDIA inference gateway; prompt caching confirmed (`cache_read_input_tokens` 2084 on a repeat) |
| Gate | `lib/replay.scm` | Proven in both directions by `make loop` |
| Publication | `define-tool`, `lib/tools/*.scm`, MCP | `search-read` published and callable over MCP; a model-written tool has published through the full loop |

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

## The model in the loop

`make synthesize` runs it once: transcripts → opportunity → model → replay →
published file. The credential is the NVIDIA inference gateway token, which fronts
many providers behind one key and speaks the Anthropic Messages API natively on
`/v1/messages` — structured output and `cache_control` breakpoints included — so
only the host, the auth header and the model name differ from talking to Anthropic
directly. Both are supported; `NVIDIA_INFERENCE_API_KEY` selects bearer auth and
`ANTHROPIC_API_KEY` selects `x-api-key`.

Replay needs the concrete calls behind a pattern, so opportunities carry `samples`:
real recorded command lines with the directory they ran in. A recorded command only
means anything in that directory, and both sides of the comparison run there.

The model is asked for three things, not one: the tool, a `translate` procedure
turning a recorded command into the tool's arguments, and a `legacy_form` procedure
rendering the tool's result as the old command printed it. Without the last two the
gate has nothing to do.

### What running it actually showed

| Run | Outcome | Why |
|---|---|---|
| `cat` | published | agreed on its case; later re-judged (see below) |
| `cat` | refused | the model's tool raised `string->list expects a string` |
| `grep -n` | refused (truncated) | 16k output budget was not enough for three procedures |
| `grep -n` | refused | 5/5 cases replayed, **0 agreed** |

That last one is the whole argument for the gate. `grep_numbered` was 4.5× smaller
(708 bytes against 3,164) and 7× faster (9 ms against 63 ms) and perfectly stable —
and wrong: the recorded commands use GNU basic-regex alternation (`\|`), which it
did not implement, so it returned nothing. Cheaper, faster, stable, and silently
empty is exactly the failure a cost metric cannot see.

### Two corrections the runs forced

- **Stability is only a win against an unstable command.** The first `cat` run
  published a tool that was *larger* than the output it replaced (5,973 bytes
  against 5,681) and no faster, because being stable counted as a win on its own.
  `cat` is already perfectly stable, so that was no win at all. Legacy stability is
  now measured by running the command twice; a candidate must still always be
  stable, but it only earns a stability win where the old command churned.
- **Replaying means running.** A corpus contains `rm -rf`, `docker`, deploys. Only
  shapes whose program reads and reports are eligible, and — since a recorded line
  matching the safe shape `head -c` turned out to be
  `cd /home/jkh && time ./toolscheme analyze ... | head -c 3000` — *every* command
  in the line must be safe, not just the one the shape came from.

## Open

- **A cost-only win is not yet distinguishable from a correctness win.** The gate
  requires equivalence on recorded cases, which is the strongest available check,
  but a pattern with few distinct samples is weakly covered.
- **Fusion patterns are the biggest measured opportunity and cannot be gated.**
  `read -> read` and `grep -> read` are agent-native tool calls, not shell commands,
  so there is no legacy process to replay against. They are reported and skipped.
- **Prompt injection.** Transcripts are untrusted input, fed to a model whose
  output becomes executable Scheme. The capability sandbox bounds the blast
  radius, and a synthesized tool should be published without process or network
  capabilities unless its pattern requires them. This needs an explicit policy
  decision before the loop runs unattended.
- **Replay proves equivalence only on recorded inputs.** The legacy path stays
  callable and publication is a file, so reverting is a deletion.
- **Platform breadth.** `platform-facts` lands here so a generated tool can branch
  on host facts; Windows and WSL adapters come after the loop is proven.
