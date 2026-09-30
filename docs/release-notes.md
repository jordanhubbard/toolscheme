ToolScheme 0.4.0 reaches Codex sessions, refuses fixed waits, and tests its own charter against the corpus it collected.

The last of those is the headline, and it is not the result the charter predicted. See `docs/relevance.md`.

Changes since 0.3.0:

## The charter, measured

- `docs/relevance.md` tests the project's founding claim -- that agents spend their budget on programs designed for a different era, and that better-shaped tools would cost less -- against 22,006 recorded tool calls and 31MB of output from live sessions. The first half holds: reading and searching dominate, and 95% of `sed` calls are line-range reads. The second half does not. That work goes through `bash`, which is 86% of output bytes, while the published tools compete with `Read` and `Grep` at 1.2% and 0%.
- Four ways for a replacement to win were measured and none survived. Bytes: `search-read` answered a real query in 3,760 bytes against 4,016 for `grep -rn -C 3`. Calls: range reads arrive batched, 3,249 calls carrying 5,608 reads, so one range per call is 73% more calls. Repeats: 12 of 1,099. Unbounded output: unbounded calls average 1,922 bytes against 4,955 for bounded ones, and none of 3,526 exceeded 100KB.
- Two structural reasons explain it. 44.2% of shell calls compose more than one program, averaging 1.8 and reaching 12 -- a shell call is a program in a language and a tool call is one operation. And the replay gate requires byte-identity, which is correct, but makes the byte count equal by construction and leaves only latency, where a fresh interpreter loses to `fork`.
- The project's own synthesized tools recorded the same thing: `read_line_range` returns 2.8% more bytes than the `sed -n` it replaces, `read_text_bounded` 3.8% more than `cat`. Both passed the gate, because the gate never required a win.
- `TOOLSCHEME_REDIRECT` is off by default and should stay off. It cannot win by construction, and it has cost an incident in which an agent was told a missing file was empty.
- `experiments/relevance/tool-layer.scm` reproduces every figure above.

## Codex

- `make install-codex-shim` installs a `codex` earlier on PATH than the real one, so sessions launch against an app server and can be continued. Codex has no turn-end hook, so there is nothing to decline; a session reachable through `codex queue` is the alternative. Three rules govern the shim: it never fails closed, it only adds `--remote` where a session is continuable, and it never execs another copy of itself -- copies are recognised by content, since resolving by path loops when a checkout's copy and an installed one are both on PATH.
- The shim survives `codex update`. `--remote` belongs to a subcommand Codex labels experimental, so the flag is checked before use and a release without it becomes a pass-through; the app server is restarted when the binary changes, since a server from the old version outlives the update. Both checks key on the binary's size and mtime, so the common path is a `stat` rather than a process.
- Continuation is bounded by a ledger rather than by the transcript. Counting continuation marks in a rollout cannot work: a failed queue leaves no trace, and Codex compacts long threads, so the count returns to zero either way. One thread was continued 431 times against a cap of 2 before this was found. `codex queue`'s exit status is now checked -- it was read from the wrong field and every failure reported success -- every attempt is recorded, and attempts are bounded at three times the delivery cap so an unreachable thread is abandoned rather than retried.
- The continuation ledger survives log rotation, reading the previous generation only when the current log does not reach back past the stale threshold.

## Refusals

- `TOOLSCHEME_REFUSE_SLEEP=1` declines a fixed wait at or over `TOOLSCHEME_REFUSE_SLEEP_MS` (10s). Measured over 20,774 calls either side of the day it was enabled: waits under the threshold went 15 to 15, waits at or over it went 32 to 2. Short polling is untouched, which is what distinguishes this from a general fall in activity. Two milder mechanisms had already failed on the same number -- an instruction in `AGENTS.md`, and hook advice that fired correctly and was read past.
- `TOOLSCHEME_DOGFOOD=<paths>` refuses, inside named trees, the tools toolscheme exists to replace, naming the equivalent each time. A refusal cannot be routed around silently: either the replacement is used or the missing capability gets written. `cat > file <<EOF` is recognised as a write rather than a read, and judgement passes over `cd`, `echo` and other transparent commands to the first program that claims to do something.

## Measurement

- `shell-parse` no longer swallows the rest of a line after a quoted word. Operator detection was suppressed while the current token carried a quote, and the flag outlived the quoted region, so `echo "hi"; ls` parsed as one command. 313 of 11,921 recorded commands parsed differently once fixed. Every downstream analysis was re-run: distributions shift, no conclusion does, and the 1,168-wait figure is untouched because those are tool calls rather than shell commands.
- Every hook rule that fires now records which one it was -- `refuse-sleep`, `dogfood`, `bound-read`, `steer`, `redirect` -- so a rule running in production can be judged afterwards. The redirect had been enabled for days with no way to tell whether it had ever rewritten a call.
- Codex continuations are recorded in the observation log, so `experiments/continuation/did-it-work.scm` can ask of both agents what each continuation produced. It separates trustworthy records from the 447 written before the exit status was checked, using the record's own shape rather than a date.
- `apropos` lists top-level bindings matching a substring, library included. The first analysis written under the dogfood rule hand-rolled a counter six times slower than the `tally` it could not find.
- `tally`, `ranked` and the `pipeline-run` in-process pipeline runner; a content cache validated by device, inode, size and mtime, admitting on second sight after a first attempt measured 3x slower than no cache at all.

## MCP

- `make install-mcp` registers the server with Claude Code and Codex through their own CLIs. This is where the measurements point -- a running server has no interpreter start to amortise -- though adoption is a separate question from availability: the tools arrive deferred and, as shaped, one operation per call, they do not beat a shell that composes.
- `search-read` accepts a source written the way its description implies. Given `"(glob \"*.cpp\")"` it previously read the text as a literal glob, matched nothing, and answered `(count 0)` with no error.

## Notes

- The publication audit runs against the repository's `lib/tools`, while agents are served from the state directory. Two synthesized tools live there without the structured `provenance` field the audit requires; their replay evidence is in a prose header. The gap is named in `tests/tools-check.scm` and belongs in whatever publishes a tool.
- `make test` and `make loop` cover different checks. Both must pass.
