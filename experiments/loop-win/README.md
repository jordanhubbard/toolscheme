# What the loop is actually worth

Measured over this machine's recorded sessions: 78,005 shell commands across
7.4 days, two agents, nine projects.

## The measurement

2,092 of those commands are plain file reads whose file still exists — `cat`,
`sed -n 'A,Bp'`, `head`, `tail`. Replayed two ways:

| | time | note |
|---|---|---|
| as the sessions ran them | **22.0 s** | one process per call |
| through toolscheme, in process | **0.24 s** | 92× faster |
| of which the content cache contributes | 0.016 s | 1.07× |

Bytes returned are identical. The agent receives the same text either way, so
there is **no token saving at all** — only wall clock.

## What that is worth

**22 seconds, over 7.4 days.**

That is the entire measured win of the observe → analyse → synthesise → gate →
publish loop on real traffic. It covers 2.7% of commands, and it is available
only through MCP, because a redirect spawns a fresh interpreter and gives the
92× straight back.

For scale, the same corpus contains **11.9 hours** of agents sleeping on fixed
timers. The thing this project was built to do is worth about two thousandths of
the thing it found by accident while looking.

## Why the win is so small

Three findings compound, each measured separately:

- **80% of commands are compound** — `f=x; grep … $f`, pipelines, redirects. No
  single-purpose tool can stand in for a shell program.
- **A further 11% are arbitrary or stateful** — `git`, `python3`, `make`, `ssh`.
- Of the 9.3% that remain, the bytes are **the work**. `cat` prints the file you
  asked for; a structured wrapper returns the same bytes with a record around
  them. There is nothing to save.

The 92× is real but it is a speedup on something that was already fast: 10.5 ms
per call becomes 0.12 ms. Nobody was waiting on it.

## What this does not measure

The parts that are not tool substitution, and which the same corpus says are
much larger: 11.9 hours of fixed-timer sleeping that `wait-for` addresses, and
34.5 hours of sessions idle after naming their own next step. Those are
behavioural, not mechanical, and the loop measured here does not touch them.

## Reproducing

```sh
# build the read sequence from the observation log, then
toolscheme replay-reads.scm --root $HOME          # in process
TOOLSCHEME_CACHE=0 toolscheme replay-reads.scm --root $HOME
```
