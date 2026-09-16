# Self-improvement loop

The purpose of the project: watch what a coding agent actually calls, measure what
it costs, write a better tool, prove it, and hand it back — so the tool collection
adapts to how agents work instead of inheriting Unix's 1970s ergonomics.

```
Claude Code / Codex transcripts ─┐
   pasted session text ──────────┤
   PreToolUse/PostToolUse hook ──┼─→ agent-log-events ─→ opportunity report
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
| Intake | `lib/agentlog.scm` | Four schemas -- two Claude transcript formats, Codex rollouts, and toolscheme's own hook -- from a path or pasted text |
| Live observation | `lib/hooks.scm`, `hooks/observe.sh` | `PreToolUse`/`PostToolUse` on Claude Code and Codex alike; ~2.5 ms per event |
| Rewriting | `lib/redirect.scm` | Verified on both agents; off by default, and claims nothing today because nothing has earned a claim |
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

## Watching instead of reading

A transcript is what was written down afterwards. A hook sees the call happen, and
that difference decides what can be measured:

| | flat transcript | nested transcript | hook |
|---|---|---|---|
| tool and arguments | yes | yes | yes |
| token usage / cache churn | no | yes | no |
| working directory | no | yes | yes |
| call id joining call to result | no | no | yes |
| duration | no | no | **yes** |

Duration is the one that matters most, because a tool's cost is count times
duration, not count. It needs a per-call timestamp *and* a way to pair a call with
its result, and no transcript format has both.

The hook is deliberately the least clever component in the project. It runs inside
someone else's session on every tool call, so it observes and nothing more: no
denial, no rewriting, no output, and exit 0 whatever happened. A record is clipped
to stay under `PIPE_BUF`, because tool calls arrive in parallel batches and two
`O_APPEND` writes larger than that interleave and corrupt each other.

## Rewriting a call, and why nothing is rewritten yet

A `PreToolUse` hook may return `updatedInput`, replacing the tool input before it
runs, so a shell command becomes a toolscheme call with no round trip and nothing
for the model to learn. Verified working on both Claude Code and Codex 0.154.0:
the rewrite is accepted, executed, and its output matches the original exactly.

One rule governs it: **a command shape is rewritten only if a published tool claims
that shape and carries evidence of reproducing it exactly and faster.** The
`grep_numbered` run is why — 4.5× smaller, 7× faster, perfectly stable, and
returning nothing at all. A rewrite to that would have been invisible.

Today it rewrites nothing, and that is the honest result rather than a gap.

I expected a large win here, from `make bench`: a process launch costs 1.89 ms
against 4.6 µs for an in-process glob over 200 files, and `grep -rn X | head -20`
is two launches. Measured end to end, the substitution is **slower** — 27 ms
against grep's 11 ms over the same cases. A fresh interpreter start costs more than
the extra process the fusion saves, and GNU grep out-scans the interpreter by
roughly ten to one on large files. There is no crossover; it gets worse as files
grow.

The first version of that measurement said the opposite — a 5× *speedup* — because
it timed `tool-render` inside the already-warm test interpreter against a
subprocess `grep`. The redirect spawns a fresh interpreter, so the only faithful
measurement runs the rewritten command the same way the agent would.

The general shape of the finding matters more than this one tool. **Reproducing a
command byte for byte leaves speed as the only axis to win on**, because identical
output cannot be cheaper output. Substituting a single shell command is therefore
only worth it when our implementation is genuinely faster, and against a mature C
tool on its own ground it usually will not be.

Where the win actually lives is where the interaction changes shape: fusing calls
that currently cost separate round trips. `read -> read` (1203) and `grep -> read`
(192) are the largest measured pairs, and a fused tool is not byte-identical to
anything, so it cannot be a transparent rewrite of one command — it needs the hook
to compare a fused call against the recorded sequence, which is the next mechanism
rather than this one.

Three fidelity details showed up on the way, each invisible until compared byte
for byte: grep prints no path prefix when given a single file, terminates its last
line, and a pipeline exits with its *last* command's status — so `grep x f` exits 1
on no match while `grep x f | head -20` exits 0.

A fourth showed up in the Codex reader. Its tool inputs use JavaScript object
literals with bare keys, so matching only `cmd:` and not `"cmd":` silently dropped
a fifth of the shell calls and a third of the `sed -n` invocations — an undercount
that reads as a finding rather than as a bug. Both spellings occur in the same
corpus.

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
