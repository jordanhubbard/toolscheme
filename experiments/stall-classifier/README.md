# Is the stall heuristic any good?

`continue.scm` decides whether a stopped agent should be told to carry on. Until
now that decision was two hand-written phrase lists, and the only evidence they
worked was that they passed tests I wrote from the same intuition that produced
them. This measures them against something independent.

## Result

162 turn-ending stalls, taken from this machine's own Codex and Claude Code
transcripts. Reference labels from Opus 5, which is told nothing about how the
phrase lists work.

| arm | precision | recall | F1 | catches questions |
|---|---|---|---|---|
| phrase list (was shipping) | 0.15 | 0.11 | 0.13 | **1 / 28** |
| classifier, one question (Haiku 4.5) | 0.76 | 0.70 | 0.73 | 20 / 28 |
| **wired path (now shipping)** | **0.83** | **0.68** | **0.75** | **24 / 28** |

The third row is `stall-signals` itself, called once per stall exactly as a hook
calls it — not a reimplementation in the harness. It beats the middle row on the
guard because it asks three differently-worded safety questions instead of one
and stops if any fires, and unions in the phrase list, which fired once in the
whole corpus and was right. Four questions cost the same round trip as one; that
is the entire reason the ensemble is affordable.

Measured over the run: 162 requests in 4m33s, 1.7s each, one timeout at an 8s
budget. Latency, not accuracy, is what keeps this off the per-tool-call path.

## Von, measured: much faster, much worse at this

Von 1.0 (395M, run locally on a GB10, same `/v1/systemone` wire API) answered
all four questions in **93ms at p50 and 147ms at p95**, against 1.7s for the
chat backend — the whole corpus in 16s instead of 4m33s, on this machine, with
no credential and nothing leaving it.

It is not usable for this decision:

| | precision | recall | F1 | catches questions |
|---|---|---|---|---|
| chat backend (Haiku 4.5) | 0.83 | 0.68 | 0.75 | 24 / 28 |
| Von, shipped thresholds | 0.00 | 0.00 | 0.00 | 26 / 28 |
| Von, best of 60 threshold/guard combinations | 0.24 | 0.92 | **0.38** | 12 / 28 |

The first Von row is not its fault. `needs_choice` — "would carrying on require
choosing between alternatives the message leaves open?" — came back ≥0.3 on 153
of 162 messages, median 0.992. It is very nearly a tautology for any agent about
to continue, and Von answers it literally where a chat model answered it
charitably. Ambiguity in a question is invisible until something takes it at its
word.

The third row is the real finding, and rewording did not move it.
`names_next_step` is saturated: p10 0.56, p50 0.96, p90 1.00. Von says a next
step is named for nearly every message in the corpus, so there is no threshold
that separates the 37 continuable stalls from the other 125. A second wording
("describe work still unfinished and about to be done next") produced p50 0.95
and the same 0.38.

**Confound, stated plainly:** the reference labels come from Opus 5 and the chat
arm is Haiku 4.5 — the same family, so some of 0.75-vs-0.38 is likely agreement
between relatives rather than truth. It would take human labels to separate
those, and this corpus has none.

So the chat backend stays the default. Von stays wired, because the gap is in
one saturated question and not in the transport, and because 93ms is in reach of
decisions the chat path is too slow to make at all — the per-tool-call ones this
project actually cares about. A frontier System One model may also behave
differently from a days-old 395M one; nothing here measures Jev.

## Laya, measured: faster still, better than Von, still not enough

| arm | p50 | precision | recall | F1 | catches questions |
|---|---|---|---|---|---|
| phrase list | — | 0.15 | 0.11 | 0.13 | 1 / 28 |
| chat backend (Haiku 4.5) | 1700 ms | **0.83** | 0.68 | **0.75** | **24 / 28** |
| Von 1.0 | 93 ms | 0.24 | 1.00 | 0.39 | 8 / 28 |
| Laya, `typed-decisions` | 54 ms | 0.23 | 0.97 | 0.37 | 0 / 28 |
| Laya, english root | **54 ms** | 0.36 | 0.65 | 0.46 | 2 / 28 |

Two of the three claims hold. Laya is the fastest thing measured here — 54 ms
p50 for four questions, against Von's 93 ms and a claimed 200 ms for Jev — and
it is better than Von at this task, 0.46 against 0.39, with real dynamic range
(p10 0.08, p50 0.58, p90 0.90) where Von was saturated. It is still nowhere near
the chat backend.

Four things were tried before concluding, and none of them closed the gap:

- **Checkpoint.** `typed-decisions` sounds right for this and is much worse
  (0.37, and it caught none of the 28 questions). The english root checkpoint is
  the one to use.
- **Criteria.** Laya documents noul as instructions-only. Supplying the same
  true/false criteria Von got made it worse: 0.42, zero questions caught.
- **State length.** Agent messages are long and full of code, markdown and
  citation XML, so out-of-distribution input was the obvious suspect. Truncating
  to the last 200 / 400 / 800 characters gave 0.40 / 0.38 / 0.36 — all worse than
  the full message. The hypothesis is not supported.
- **Thresholds.** Swept, and there is no operating point that is both safe and
  useful:

| guard threshold | precision | recall | F1 | fires | catches questions |
|---|---|---|---|---|---|
| 0.10 | 0.40 | 0.05 | 0.10 | 5 | 28 / 28 |
| 0.20 | 0.47 | 0.19 | 0.27 | 15 | 19 / 28 |
| 0.50 | 0.31 | 0.41 | 0.35 | 48 | 4 / 28 |
| 0.90 | 0.36 | 0.65 | 0.46 | 67 | 2 / 28 |

Catch the questions and it continues almost nothing; make it useful and it walks
past 26 of the 28 messages that wanted a person. The chat backend does both at
once.

**What both small models actually fail at** is the same thing, and it is the
guard rather than the decision: recognising that a message asks the human
something. Laya scores "Which OS should I build for first?" at 0.155. That is
not a calibration problem a threshold can fix.

That leaves a clean split rather than a winner. These models are fast enough for
the per-tool-call path, where the chat backend is disqualified at 1.7s, and the
questions there ("will this command emit a large unstructured blob?") are much
closer to what they do well. The stall decision needs pragmatic reading of a
long message, and for now that means the slow model.

The phrase list fires on 26 stalls and is wrong on 22 of them, while missing 33
of the 37 that were genuinely continuable. It is not a weak signal; it is very
nearly no signal. And the guard I called the one that must never regress --
a question stops everything -- catches one question in twenty-eight.

## Why it fails, concretely

Reading the disagreements is more useful than the table:

- *"The client **remains open**. No source changes made yet."* — `"remains open"`
  matched an application window being open, not work remaining.
- *"...**which OS?**"* followed by an `<oai-mem-citation>` block. `asks-a-question?`
  looks only at the last 400 characters, and Codex appends citation XML after the
  prose, so the window never sees the question mark. This one mechanism explains
  most of the 1/28.
- *"Awaiting diff review before running..."*, *"Then tell me to retry."* — real
  requests for a human, carrying neither a `?` nor any listed phrase.
- *"...remain tracked follow-ups"* — `"follow-up"` matched work deliberately
  deferred, which is the opposite of a next step.

Surface strings do not carry the sense. That is the whole finding.

## What it does not show

- The reference is a model, not a person. Sampled disagreements were read by hand
  and the reference was right in every case checked, but that is a spot check.
- Reference and cheap arm share prompt wording, so some agreement may be an
  artifact of the phrasing rather than of the task.
- One machine, one user, mixed projects, n=162.
- 4 of 28 questions still slip past the wired path. Much better than 27, and
  still four sessions told to carry on when a person was wanted. The cap is what
  bounds the damage, not the classifier.
- The ensemble was the cheapest win here (20/28 to 24/28 for no extra round
  trip). More questions would probably help again; nothing about the pricing
  argues against asking ten.

## Reproducing

```sh
python3 extract.py                       # transcripts -> /tmp/stalls/corpus.jsonl
toolscheme predict.scm --root /tmp/stalls --lib ../../lib --text \
  > /tmp/stalls/predictions.tsv          # the shipping predicate, not a copy of it
python3 label.py                         # reference labels
ARM_MODEL=azure/anthropic/claude-haiku-4-5 ARM_OUT=/tmp/stalls/labels_cheap.jsonl \
  python3 label.py
python3 score.py "classifier=/tmp/stalls/labels_cheap.jsonl"
```

`label.py` batches ten messages per request and asks two constrained questions
about each, which is the shape a System One model takes natively. Pointing
`ARM_MODEL` at one is the only change needed to add it as a third arm.
