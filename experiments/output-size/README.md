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
