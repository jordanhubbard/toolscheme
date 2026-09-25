# The corpus, measured with the tokenizer instead of a regex

Every earlier analysis in this project's history was a Python heredoc that
decided whether a command was compound by looking for `|`, `&&`, `;`, `$`,
`` ` ``, `>` or `<` anywhere in the line. Dogfood mode refused the heredoc, so
the analysis was rewritten here in toolscheme, using `shell-parse` — the POSIX
tokenizer this project already had, written precisely because a regex sweep over
this same corpus once reported `e`, `if` and `out.field` as the top commands.

The tokenizer disagrees with the regex about a quarter of the corpus.

## The disagreement

| | commands called compound |
|---|---|
| regex | 6,907 (73%) |
| `shell-parse` | 4,735 (50%) |
| **regex wrong** | **2,417 — 26% of the corpus** |

What the regex was reading:

```
ssh jkh@100.72.16.110 'tail -n 20 /Users/jkh/nanolang-qualification.log'
gh run view 36180207448 --json status,conclusion --jq '{status: .status}'
ssh jkh@100.72.16.110 'for p in $(pgrep -P 69738); do ps -p "$p"; done'
```

The pipes, semicolons and `$` are inside **quoted arguments** — a remote script,
a jq expression. Locally each is one command. A regex sees the characters; the
tokenizer sees the quoting.

## The number that changed

The substitutable ceiling was reported as 9.3% on the regex reading. Measured
properly:

| | share |
|---|---|
| single command, only replaceable text tools | 7% |
| **pipeline, only replaceable text tools** | **17%** |
| **ceiling together** | **24%** |
| single command, other programs | 39% |
| compound, other programs | 29% |
| embeds a program (heredoc) | 7% |

Two things follow, and both cut against what was concluded before the rewrite.

**The ceiling is 2.6x larger than reported.** 24% of commands are composed
entirely of text tools toolscheme already has.

**Most of it is pipelines, not commands.** 17 points of the 24 are lines like
`grep -n x f | head -20` — several processes where one would do. The unit worth
replacing is the line, not the command in it, which is the opposite of what the
single-command substitution work assumed.

## Cost

9,471 commands, full POSIX tokenization of every one: **1.25 s**. The Python it
replaced was slower and wrong.

## Running them

```sh
toolscheme shapes.scm             --root $HOME --lib ../../lib
toolscheme regex-vs-tokenizer.scm --root $HOME --lib ../../lib
toolscheme ceiling.scm            --root $HOME --lib ../../lib
```

They read `$XDG_STATE_HOME/toolscheme/observations.jsonl` — the current log
only, not the rotated generations.

## What was awkward, and what it turned out to be

These scripts were first written with a hand-rolled `tally-add` that rebuilt an
association list per key — `collections.Counter`, badly. It was recorded as a
missing primitive.

It was not missing. `tally`, `tally-by`, `ranked` and `top` were three files
away in `prelude.scm`, and `tally` is a sort-and-group: O(n log n) against the
hand-rolled O(n·k). Measured on the same 9,489 items, **66 ms against 390 ms**,
for the identical answer.

So the gap was never functionality. It was that `primitive-names` lists only
what C++ installed, and everything that makes the library pleasant to use is
Scheme and was invisible. The fix is `(apropos "tally")`, which searches every
top-level binding — 592 of them — and would have answered the question in one
call.

That is a better outcome than adding a primitive, and it was only reachable by
being forced to write the analysis in the tool rather than around it.
