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

A fifth is structural rather than cosmetic: a `PostToolUse` event carries the
input that actually ran, so after a rewrite the log records toolscheme's own
invocation as though the agent had chosen it. Those records are marked and their
commands excluded from the shape rankings, or the loop starts measuring its own
output and ranking it as demand.

A fourth showed up in the Codex reader. Its tool inputs use JavaScript object
literals with bare keys, so matching only `cmd:` and not `"cmd":` silently dropped
a fifth of the shell calls and a third of the `sed -n` invocations — an undercount
that reads as a finding rather than as a bug. Both spellings occur in the same
corpus.

## Where it has to live

A hook inside a repository can only report on that repository, because the agent
configuration names an absolute path to it. `make install` puts the binary, the
library and the hook under a prefix outside any checkout and prints the
configuration to add; observations go to `$XDG_STATE_HOME/toolscheme`, which is
also the sandbox root the hook runs under, so it watches every project and can
write to none of them.

There is no daemon, and collection does not need one: each hook invocation is a
short-lived process costing about 2.5 ms. A resident service would remove that
start-up, which matters in two places and neither is collection. It would improve
rewriting -- though not enough to have saved `search-read`, which is still slower
than grep once its start-up is discounted. And it is close to a prerequisite for
steering, because deciding what to tell a session on the basis of what other
sessions have already done means having that history in memory rather than
re-reading a growing log on every tool call.

## What the corpora say

Both agents, analyzed after the loop could finally read them at full size.

| | Claude Code | Codex |
|---|---|---|
| tool calls | 7,097 (158 sessions) | 13,131 (30 sessions) |
| result bytes | 127 MB | 40 MB |
| **bytes per call** | **18 KB** | **3 KB** |
| busiest shape | `head -20` (274) | `sed -n` (3,215) |
| redundant calls | 541 | 2,149 |
| tool time | not recorded | **52.8 hours** |

The two waste different things. Claude Code wastes volume -- six times the bytes
per call, one file read sixty-three times. Codex wastes time.

And where Codex's time goes is the most useful number in this project so far:

| tool | time | share |
|---|---:|---:|
| `exec` | 42.2 h | 80.0% |
| **`sleep`** | **9.3 h** | **17.7%** |
| `send_message` | 0.9 h | 1.7% |

Nearly a fifth of all tool time is spent sleeping, and the most repeated
invocations in the entire corpus are identical sleeps: 55 s x 221, 45 s x 123,
50 s x 98, 60 s x 92. Roughly eight hours in the top four alone, in fixed
increments. Beside them sit 1,318 `tools.write_stdin` calls with `"chars":""` --
writing nothing to an interactive session to see whether it has finished.

That is polling, and it is the largest single opportunity measured anywhere in this
work. **A wait-for-condition primitive** -- block until a file changes, a process
exits, a port opens, a pattern appears -- replaces a 55-second guess with an event.

It is also the right *kind* of tool, which matters more than the size of the win.
Reproducing a shell command byte for byte leaves speed as the only axis and loses
to a mature C tool; waiting on an event reproduces nothing and changes the shape of
the interaction, which is where every real win in this data lives. Filesystem
watching pays for itself here rather than as a read cache, where it would be
competing with the page cache for microseconds.

`sed -n` at 3,215 is the other candidate worth taking seriously: Codex reads file
ranges through `sed` where Claude Code has a native read tool, and a ranged read is
not a tight scanning loop we would lose.

## Running the intervention instead of arguing from the corpus

Everything above is an argument from observation, and observation cannot separate a
missing tool from a missing instruction. `tests/steering-experiment.sh` runs the
same task with the per-call advice on and off and reads the result out of the
observation log.

Three attempts, each of which failed in a way worth keeping:

1. **The treatment was never delivered.** Zero advice reached any trial, because
   Codex silently skips a hook it has not been asked to trust, and the SessionStart
   hook had just been added. The arms looked identical and the null result meant
   nothing. The harness now refuses to report unless it can show the treatment
   arrived.
2. **The trigger did not match the behaviour.** The agents polled with `sleep 1` in
   a loop, thirty-six times, and the threshold was tuned to the corpus's 45-to-60
   second sleeps. A polling loop is now its own trigger. Two parser gaps fell out
   of the same run: a subshell was not tokenized at all, so `(sleep 25; touch x) &`
   parsed its first command as `(sleep`, and backgrounding was not tracked, so once
   subshells *were* parsed, every backgrounded setup looked like a wait.
3. **The control was not a control.** Both arms adopted the tool, because the
   instruction also lives in `AGENTS.md`, which every session loads whatever the
   flag says.

The third failure invalidated the comparison, so it was run again with each arm in
its own `CODEX_HOME`: two channels crossed, three trials each.

| | delivered | adopted the tool | polled in a loop |
|---|---|---|---|
| neither | 0/3 | 0/3 | **3/3** |
| hook only | 3/3 | 3/3 | **3/3** |
| note only | 0/3 | 3/3 | **0/3** |
| both | 0/3 | 3/3 | **0/3** |

Left alone, the agent polls. Every trial, identically:
`(sleep 25; touch ONE) & while [ ! -e ONE ]; do sleep 1; done`. The tool existing
and being on PATH changes nothing on its own -- nobody reaches for what they have
not been told about.

Either channel fixes the adoption. Only one fixes the behaviour. The hook is
reactive: it fires once the agent has already written the polling loop, so the
loop still runs and only the *second* wait improves -- which is why the hook arm
adopts in 3 of 3 and still polls in 3 of 3. The note is preventive: with it there,
the agent never writes the loop at all, the hook has nothing to correct, and its
advice is never delivered.

So the note dominates: it removes the behaviour rather than correcting it, costs
nothing per call, and needs no hook trust. The hook earns its place only where the
agent's instructions cannot be edited -- and there it demonstrably works.

This corrects the conclusion drawn from the contaminated run, which was that the
per-call advice added nothing measurable. Against a clean control it takes adoption
from 0 of 3 to 3 of 3. What it cannot do is get there first.

## Storage and deployment

The Git-backed deployment is implemented in `scripts/learning.py`; see
`docs/learning.md`. Hosts publish immutable session summaries and steering
revisions periodically, and startup reads a bounded local snapshot without IPC
or a network dependency. DuckDB remains optional for local raw-log analysis.
The following measurements motivated the split between capture and analysis;
the centralized database service discussed below is not implemented or required.

The log is 33 MB after a day and a half -- roughly 22 MB a day, 650 MB a month --
and a full analytical pass over 86,000 records already takes 24 seconds. That is
not a crisis and it is the wrong shape to keep scanning.

Two access patterns, and they pull in opposite directions:

- **Capture**: one tiny append per hook invocation, from several concurrent
  sessions, inside a 2.5 ms budget on every tool call an agent makes.
- **Analysis**: whole-corpus aggregation -- group, sort, count -- over everything
  ever collected.

The second wants a columnar store. The first wants nothing at all.

**The capture path must not depend on a database being up.** A hook that needs a
connection turns a database outage into a failure on every tool call in every
session on the machine, and the one rule this hook has is that it must never break
the session it is measuring. An `O_APPEND` write to a file is crash-safe, needs no
coordination between sessions, and cannot fail in a way that matters. So the file
stays as the write-ahead capture, and a database becomes the *analysis* store,
loaded from it rather than written to directly.

That split also decides what can be containerized. The hook cannot be: it is
invoked by the agent, on the host, and has to be quick and always present. What
containerizes cleanly is everything downstream -- the analyzer, the MCP server, the
store -- which is also the part worth deploying with Helm and sharing between
machines.

For the store itself, the access pattern points at a column store with real JSON
support. **DuckDB** fits the local case exactly: embedded, no server, reads JSONL
directly, and would take that 24-second pass to well under a second with nothing to
deploy. **Postgres with JSONB**, or **ClickHouse** at volume, fits the service case
where several machines report into one place.

The cost to weigh is that this project has no external dependencies at all, which
is why `md5`, `sha1`, `sha256` and the diff are in-tree. A database client would be
the first, and it should be optional at build time the way the HTTP adapter is --
`toolscheme` itself must keep working, and keep collecting, with nothing installed.

## Bounding a read, and what it cost

The premise held where the agent was naive and nowhere else. Against a disciplined
use of Unix tools -- grep for line numbers, then ranged reads -- a structured result
is byte-neutral: 8,111 against 7,899 on one query. Against the naive pattern it is
not close: 680 KB against 8 KB for the same question.

Claude Code is the naive one. Asked a question about two documents it issued two
whole-file reads, 25,786 bytes, to answer in eighty words. Bounding those reads to
120 lines -- substitution through `updatedInput`, not advice -- returned 4,907
bytes, **81% fewer**, and both answers were substantively the same.

Two honest limits on that. The answer happened to be near the top of both files; a
question about line 400 would have been damaged, and a bare `Read(path)` carries no
statement of what is wanted, so nothing downstream can bound it except by guessing
that the answer comes early. And it is one trial. The mechanism is what the earlier
experiment predicted: told to *prefer* a tool, an agent adds it and carries on;
substitution is the only thing that removes a step.

## Where the time actually goes

Under a throughput objective rather than a token one, the ranking changes
completely. Across 30 Codex sessions:

| | hours |
|---|---:|
| idle, waiting for a human | **34.5** |
| of which in recoverable-length gaps (1--60 min) | 10.1 |
| sleeping on a fixed timer | 2.7 |

Idle time is twelve times the sleeping problem that `wait-for` was built for.

But the tempting reading of that is wrong, and the data says so. Of 141
recoverable-length stalls, **2** ended with a question or an offer to proceed. The
other 139 ended with a completion summary. The agent was not blocked awaiting a
rubber stamp; it had finished and was waiting for the next instruction. Auto-
approving obvious choices would recover two stalls, not a hundred and forty.

What those summaries do contain is the next step, named by the agent itself: "Full
borrow task718 remains open. Next required slice: nested resource projections with
place identity and overlap tracking." That is not a decision needing approval. It
is an agent that knows what to do next and stops anyway, which is a far larger
target -- and a far more dangerous one, since an agent that never stops is an agent
with no natural place to check its own work.

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
