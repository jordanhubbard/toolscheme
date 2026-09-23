# Can anything predict how much a command will print?

This is the question toolscheme's per-call path actually needs answered. A
bounded, structured replacement for `grep -rn … | head -20` only pays for itself
on calls that would otherwise dump a lot of text, so knowing *before* the call
which ones those are is what a redirect decision rests on.

It is a better corpus than the stall one in the way that matters: the label is
recorded rather than judged. Output size is a fact on disk, so nothing here
depends on a model's opinion of another model.

**3,560 distinct shell commands** with their real output sizes, from this
machine's Claude Code and Codex transcripts. 21% printed 2 KB or more.

## Result

| predictor | precision | recall | F1 | accuracy | cost |
|---|---|---|---|---|---|
| always "small" | 0.00 | 0.00 | 0.00 | 0.79 | — |
| always "large" | 0.21 | 1.00 | 0.35 | 0.21 | — |
| **first program, mean bytes seen before** | 0.45 | 0.77 | **0.57** | 0.75 | **0 ms** |
| pipeline ends in a bounding tool | 0.27 | 0.86 | 0.41 | 0.49 | 0 ms |
| Laya noul, best threshold | 0.24 | 0.83 | 0.37 | 0.42 | 21 ms |
| Laya score, best threshold | 0.21 | 0.99 | 0.35 | 0.22 | 21 ms |
| memo, Laya only for unseen programs | 0.45 | 0.77 | 0.57 | 0.75 | — |

A lookup table keyed on the program name, leave-one-out, beats the model by a
wide margin and costs nothing. Laya barely separates from "always large", whose
0.35 is what guessing buys. Its precision of 0.21–0.25 is the base rate, which
is the signature of no signal at all.

The hybrid row is the one that settles it: letting Laya decide only the commands
whose program had never been seen before changed nothing to two decimal places.
There is no residue for it to pick up.

## It is not the harness

Asked directly, on the least ambiguous cases available, it is close to
anti-correlated:

```
0.120  find / -type f        <- unbounded walk of the filesystem
0.178  grep -rn foo .
0.463  echo hi               <- two bytes
0.645  cat /var/log/syslog
0.725  wc -l *.c             <- prints one line per file
```

`find /` scores lowest and `wc -l` highest. Both the noul and the ordinal score
primitive were asked, in one forward pass, and both behaved this way.

## Is there anything beyond the program name?

The memo keys on the program alone and its precision is 0.45, so it cannot tell
`grep -rn x /` from `grep -n x small.c`. If a capable model reading the whole
command cannot beat it, the table is at the ceiling for this task and no faster
or larger local model is worth chasing. 400 commands sampled from the corpus,
judged by Haiku 4.5 with the arguments in front of it:

| predictor | precision | recall | F1 | accuracy |
|---|---|---|---|---|
| memo (program name only) | 0.42 | 0.77 | **0.55** | 0.76 |
| Haiku reading the arguments | 0.24 | 0.29 | 0.26 | 0.69 |
| memo OR model | 0.32 | 0.81 | 0.46 | 0.63 |
| memo AND model | 0.51 | 0.25 | 0.33 | 0.81 |

**Headroom: −0.29 F1.** Reading the arguments makes it worse, not better. Where
the two disagree, on 157 of 400, the memo is right 92 times and the model 65.

The one thing the model adds is precision when it agrees: "memo AND model"
reaches 0.51, the best of any row, at a quarter of the recall. If a future
rewrite needs to be very sure before substituting, that intersection is the
shape to reach for — but not at 1.7s a call, and not for F1 0.33.

So the answer is that output size is mostly unpredictable from the command text
beyond which program runs. That is a fact about the task, not about any model,
and it closes the question: there is nothing here for a better local model to
recover.

## Two harness bugs worth naming

Both produced results that looked like findings.

The first run reported the model at 0.00 precision and 0.00 recall. Every batch
was failing on an exhausted budget and the harness padded the missing answers
with False, which is indistinguishable from a model that thinks nothing is ever
large. Failures are now retried and then fatal.

The second assigned the batch result before checking its length, so when a batch
came back with 19 answers for 20 commands and every retry failed the same way,
the short batch stayed — shifting every later prediction onto the wrong command.
Answers are now keyed by an index the model echoes back, so a dropped or
duplicated entry is caught instead of silently misaligning the labels.

## What this means for the per-call path

No classifier goes on it. The question was whether 21 ms could buy a decision
that 1.7 s could not, and the answer is that the decision does not need a model:
the program name and its history predict output size better than either.

That history is exactly what toolscheme already records. The observation log is
a better source for this than any model measured here, it is free to consult,
and it improves as the log grows — which is the loop this project was built
around, arriving at an answer that argues against adding a model rather than for
it.

Both experiments now point the same way. These typed-decision models are
genuinely fast and not yet useful for either decision toolscheme needs: the
stall decision wants pragmatic reading of a long message and still goes to a
remote model at 1.7 s, and the per-call decision wants a lookup table.

## Reproducing

```sh
mkdir -p /tmp/sizes
python3 extract.py                       # transcripts -> corpus with real sizes
python3 baselines.py                     # the free predictors
HF_HOME=... python3 predict-laya.py      # needs the laya venv
```

`THRESHOLD` (default 2000 bytes) sets what counts as a large output.
