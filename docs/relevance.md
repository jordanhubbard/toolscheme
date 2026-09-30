# Is this project relevant?

The charter says agents spend their budget on programs designed for a different
era, and that better-shaped tools would cost less. That is a claim about a
corpus. The corpus is now large enough to test it: **22,006 recorded tool calls,
17,540 of them carrying a shell command, and 31MB of tool output** from live
sessions on one machine.

Every figure here comes from `experiments/relevance/tool-layer.scm`, which reads
the observation log and can be re-run.

The short answer: **the tool-replacement thesis is not supported by this
corpus. The instrument and the refusals are.**

## What the corpus says

Output bytes, by the agent's own tool:

| tool | bytes | share |
|---|---:|---:|
| Bash | 26.8 MB | 86% |
| Edit | 3.2 MB | 10% |
| Read | 358 KB | 1.2% |
| Grep | 0 | 0% |

And within Bash, by program:

| program | bytes |
|---|---:|
| sed | 7.1 MB |
| rg | 4.7 MB |
| gh | 3.3 MB |
| git | 3.1 MB |
| ssh | 2.8 MB |

So the premise's first half holds: reading and searching dominate. `sed`, `rg`,
`tail`, `cat` and `nl` together are about half of all shell output, and 95% of
the `sed` calls are `sed -n` line ranges -- exactly what `read_line_range` does.

The second half does not hold. The published tools compete with `Read` and
`Grep`, which are 1.2% and 0% of the spend. The work is going through `bash`.

## Four ways to win, measured

**Fewer bytes.** `read_line_range` returns the same lines `sed -n` returns, plus
structure. It is not smaller. For search-then-read specifically, `search-read`
answered a real query in 3,760 bytes against 4,016 for `grep -rn -C 3` -- 6%,
which is noise.

**Fewer calls.** The range reads arrive batched: 3,249 calls carrying 5,608
reads, 1.7 to a call and up to 8. One range per call turns 3,249 calls into
5,608. That is 73% *more* calls.

**Eliminating repeats.** 12 exact repeats out of 1,099 range reads: 0.7% of the
bytes. There is nothing there.

**Bounding unbounded output.** The charter says `| head -N` is a workaround for
tools that cannot bound themselves, so unbounded calls should be the expensive
ones. They are the cheap ones -- mean 1,922 bytes against 4,955 for bounded
calls, and not one of 3,526 exceeded 100KB.

## The loop already said so, in its own artifacts

Two tools were synthesized, gated by replay, and published to the state
directory, where both agents are served them over MCP today. Each records what
the replay measured in its own header:

| tool | replaces | legacy bytes | candidate bytes |
|---|---|---:|---:|
| `read_line_range` | `sed -n 'A,Bp'` | 52,677 | 54,172 (+2.8%) |
| `read_text_bounded` | `cat PATH` | 45,229 | 46,942 (+3.8%) |

Both passed. The gate asks for equivalence and stability, and both are equivalent
and stable; it does not require the replacement to be smaller. So the system
wrote down, in the file it published, that its replacement costs more than the
command it replaces -- and published it anyway.

That is not a bug in the gate so much as the same structural fact seen from the
inside. A replacement that must reproduce the original exactly cannot return less
than the original, and the structure it adds to make the result typed is the
extra 3%.

Two smaller findings came out of the same place. Neither synthesized tool carries
the structured `provenance` field the publication audit requires -- the evidence
is in a prose header instead -- and the audit never noticed, because it runs with
`TOOLSCHEME_STATE=` cleared and so sees only the repository's own `lib/tools`,
while what agents are served comes from the state directory. A gate that does not
look at what is actually served is not covering production.

## Why substitution cannot win, structurally

Two reasons, and both are properties of the design rather than of the
implementation.

**A Bash call is a program; a tool call is an operation.** 44.2% of calls compose
more than one program, averaging 1.8 and reaching 12. `a && b | c` is one call.
Matching it takes as many tool calls as it has stages. The shell gives agents
composition for free, and a per-operation tool cannot price-match a language.

**Byte-identity forecloses the only axis left.** The replay gate requires a
substitution to reproduce the original's bytes exactly, which is the right rule
-- an agent cannot see that a rewrite happened, so it must not be able to tell.
But reproducing the bytes exactly means the byte count is equal by construction,
leaving latency as the only thing to win on. A fresh interpreter costs 13-20ms
against roughly 1ms to `fork`. The gate and the win are mutually exclusive.

This explains four separate failed attempts at redirection that were each
diagnosed as a tuning problem.

## What is relevant

**Refusal works, and nothing else did.** A fixed wait of ten seconds or more is
the largest single waste measured. An instruction in `AGENTS.md` did not change
it -- sleeping continued at 370, 190, 155, 87 and 125 a day afterwards. Hook
advice fired correctly and was read past. A `permissionDecision: deny` moved it:

| | before | after |
|---|---:|---:|
| waits under the threshold | 15 | 15 |
| waits at or over it | 32 | 2 |

Short polling, which the rule deliberately leaves alone, did not move at all --
which is what distinguishes this from a general fall in activity. The
observation is recorded before the decision, so these are *requests*: agents
stopped asking, rather than merely being blocked.

**The instrument works.** Every number on this page, including the ones that
refute the project's own charter, came from the observation log and the analysis
primitives. A tool layer that cannot beat `sed` still measured, precisely, that
it cannot beat `sed` -- and named the structural reason. That is worth having,
and it is worth having *because* it is willing to return this answer.

## Does it pay its way?

Relevance and economics are different questions. The first asks whether the tools
are better; the second asks whether the whole apparatus returns more than it
costs. Both are measurable here.

The hook is a tax on every tool call. `PreToolUse` must block, because it can
refuse; `PostToolUse` follows. Measured against a 22MB log, each costs 16-17ms
and neither grows with the log. Across the corpus that is 32,069 hook
invocations, or **23ms per tool call**.

The refusal is the only mechanism with a measured saving. Normalised per call,
so the two windows are comparable:

| | ms per call |
|---|---:|
| time in fixed waits before the refusal | 69 |
| time in fixed waits after | 11 |
| **saved** | **58** |
| **hook cost** | **23** |
| **net** | **35 saved** |

So the runtime ledger is positive, by about 2.5 to 1. Over the 9,141 calls since
the refusal was enabled that is roughly 530 seconds saved against 210 spent --
call it five minutes, over four days.

That is a thin margin, and it is worth saying plainly that it took a defect fix
to get there. Until this was measured, `continue-decision` computed its stall
signals before checking whether continuation was enabled, so every turn end paid
an HTTPS round trip -- 800ms, against 15ms once the check moved -- for a decision
that had already been made. Nothing in the test suite could see it, because the
tests call the decision directly and were right about what it returned.

Against that sits the code, sized by what the evidence says of it:

| | lines |
|---|---:|
| pays (observation, analysis, refusals) | 1,460 |
| unproven (continuation, classification) | 847 |
| disproven (substitution, replay, synthesis, MCP, tools) | 1,661 |
| the interpreter all of it runs on | 10,449 |

The part that pays is a tenth of the C++ it needs. A shell script in a
`PreToolUse` hook could refuse `sleep 40` in fifty lines -- though not reliably:
44% of calls compose more than one program, and a `sleep` behind `&&` is
invisible without a real tokenizer. The parser earns its place. The other 322
primitives are earning theirs somewhere else.

Where they earn it is analysis. Every figure in this document came from ad-hoc
queries written against the corpus in a language that was already there. That is
the case for the interpreter, and it is not the case the charter made.

## What follows

Neither "delete the project" nor "keep going as planned".

- The redirect should be off. It cannot win by construction, and it has already
  cost one incident where an agent was told a missing file was empty.
- The MCP tools, as shaped, will not be adopted, and should not be argued into
  adoption. If they are to compete they must take composed work in one call --
  which `toolscheme_eval` already does, and the per-operation tools do not.
- Refusals are the part with evidence behind them. The measured ranking of waste
  is where the next one should come from, not from intuition.
- The honest framing of this project is an instrument that can also enforce
  policy -- not a replacement tool layer that happens to keep logs.

## The recommendation

Feed a smaller one.

Keep the 1,460 lines that pay and the interpreter they run on: observation,
analysis, and refusals. The refusal is the only mechanism here that changed a
number two milder attempts could not move, and analysis is what produced every
finding on this page, including the ones against the project itself.

Delete the 1,661 lines of substitution -- redirect, replay, synthesis, the
published tools, the MCP serving of them. Not because they are badly built: the
replay gate is careful, and it correctly published tools whose own headers record
that they cost more than what they replace. They cannot win, for a reason that is
structural and now written down, and keeping them costs review attention on every
change and has already produced one incident.

Park continuation rather than deleting it. The Codex path works and is
background; the Claude Code path has fired zero times in 64 turn ends. Give it a
bounded window -- a fortnight of real use -- and judge it on
`did-it-work.scm` rather than on how interesting it is.

The honest summary is that the runtime ledger is positive and small, and the
engineering ledger is not. In a single session this machinery produced a
431-continuation runaway, an 800ms tax on every turn for a disabled feature, a
recorder that silently recorded nothing, a tokenizer that swallowed the rest of
the line after a quoted word, and a cap that reset itself on log rotation. Every
one was found by measuring rather than by testing. A smaller system has fewer
places for that to happen, and the part worth keeping is the part that found
them.
