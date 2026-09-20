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
