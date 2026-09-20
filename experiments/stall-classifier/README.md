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
| phrase list (shipping) | 0.15 | 0.11 | 0.13 | **1 / 28** |
| classifier (Haiku 4.5) | 0.76 | 0.70 | 0.73 | 20 / 28 |

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
- 8 of 28 questions still slip past the classifier. That is much better than 27
  and still not good enough to run unattended on the strength of one question.
  With constrained output cheap enough to ask freely, the answer is an ensemble
  of differently-worded safety questions requiring unanimity, not a single call.

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
