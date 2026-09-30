ToolScheme 0.5.0 removes the substitution stack, because it was measured and it cannot win.

This is a smaller release than 0.4.0 in every sense: fewer lines, fewer primitives, fewer claims. `docs/relevance.md` is the argument; `experiments/relevance/tool-layer.scm` reproduces the figures.

Changes since 0.4.0:

## Removed

- `replay.scm`, `synthesis.scm`, `pipeline.scm` and `lib/tools/` are gone, along with the rewrite half of the hook and the `define-tool` / `tool-manifest` / `tool-invoke` registry in C++ that existed to serve them. The Scheme library drops from 4,429 lines to 2,768; the primitive count from 322 to 319.
- `TOOLSCHEME_REDIRECT` no longer exists. It had already been switched off on the evidence.
- The reason is structural rather than a tuning failure. A shell call is a program in a language while a tool call is one operation, and 44.2% of recorded calls compose more than one program, averaging 1.8 and reaching 12. And the replay gate required byte-identity -- which is the right rule, since an agent cannot see that a rewrite happened -- but that makes the byte count equal by construction and leaves latency as the only axis, where a fresh interpreter loses to `fork`. The project's own synthesized tools recorded returning 2.8% and 3.8% *more* bytes than the commands they replaced, and passed the gate anyway, because it never asked for a win.
- `redirect.scm` becomes `decide.scm` and keeps what it was actually doing on every call: refuse, advise, bound, and record which of those happened. `credential.scm` is new, holding the key-selection logic the classifier still needs and synthesis no longer exists to own.
- MCP serves `toolscheme_eval` alone -- the one shape that takes composed work in a single call, which is the only shape that could compete. Whether an agent ever reaches for it is a separate and open question: registered with both agents for a day, it was offered to two sessions and called by none.

## Fixed

- A turn end cost about 800ms with continuation *disabled*. `continue-decision` computed its stall signals in the `let*` above the `cond`, so they ran before anything had decided whether they were wanted -- and with the classifier on, computing them is an HTTPS round trip. The cheap refusals now come first and the signals are reached only when a continuation is genuinely possible: ~15ms. Every session on this machine had been paying that on every turn, for a decision already made.

## Measured

- The apparatus was priced. The hook costs 16-17ms per event against a 22MB log, flat in log size, or 23ms per tool call across the corpus. The sleep refusal is the only mechanism with a measured saving: normalised per call, time in fixed waits fell from 69ms to 11ms. Net 35ms saved per call, about 2.5 to 1.
- Sized by what the evidence says of it, the code was 1,460 lines that pay, 847 unproven, and 1,661 disproven, on a 10,449-line interpreter. This release deletes the third category.

## Unchanged

- Observation, analysis, and the refusals. Fixed waits at or over the threshold went from 32 to 2 while short polling stayed at 15, after an instruction in `AGENTS.md` and hook advice had both failed to move that number.
- Codex continuation, which runs in a background timer and is bounded by a ledger rather than a transcript. The Claude Code continuation path is off: 64 turn ends, 0 continuations, and it is not free.
