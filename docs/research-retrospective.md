# ToolScheme research retrospective

This is a record of a negative result, written on 2026-10-25 at revision
`8d27c10`. It distinguishes measured behavior from proposed explanations and
from work someone *could* undertake. It is not a development plan.

## The original questions

The [README](../README.md) states the product hypothesis: observe coding-agent
tool calls, measure their cost, write a better tool, prove it by replay, and
publish it over MCP. The [original interpreter mission](roadmap/active-work.md)
specified a small, fast C++17 Scheme runtime with native strings, random-access
lists, structured results, and capability-limited access to host operations.
The runtime also served as the analysis language for the observation loop.

The practical question was whether a tool shaped for agents would reduce output,
round trips, or elapsed time enough to justify its integration and maintenance
cost. A later hypothesis was that an observation hook could instead reduce wasted
waiting by refusing specific commands.

Another original premise was that ToolScheme could retain local file contents or
an indexed representation across calls, notice changes made by other programs,
and answer later reads and searches with less I/O. This needs to be judged
separately from the MCP tool-shape hypothesis.

## Evidence and methods

| Question | Method and source | Result | Limit |
|---|---|---|---|
| What did agents call? | Parse live hook records and classify shell syntax with `shell-parse`; see [corpus method](../experiments/corpus/README.md) and [tool-layer analysis](../experiments/relevance/tool-layer.scm). | The [historical analysis](relevance.md) covered 22,006 calls, 17,540 with shell commands, and 31 MB of output. Reads and searches dominated output, but usually arrived through Bash. | One machine and its projects; the corpus is observational. Its current rotated log is a different window. |
| Could one-command tools replace shell calls? | Replay the same reads; compare exact bytes, call shapes, and process cost. See [replay method](../experiments/loop-win/README.md) and [historical analysis](relevance.md). | Published range/text readers added 2.8% and 3.8% response bytes. The replayed in-process reads were 92 times faster than spawning individual processes, but saved only 22 seconds over 7.4 days and no output bytes. | That 92-fold number applies to the replay subset, not complete agent tasks. It did not include the cost of persuading agents to use the tools. |
| Did the shell-shape estimate survive a better parser? | Compare regex classification with the project's POSIX tokenizer; see [corpus method](../experiments/corpus/README.md). | Regex misclassified 2,417 of 9,471 commands. The estimated ceiling for commands made solely of replaceable text tools rose from 9.3% to 24%, mostly pipelines. | A ceiling is an opportunity estimate, not a realized saving. The updated estimate did not rescue per-operation replacement. |
| Did advice change fixed waits? | Compare requests before and after an instruction, advice, and then refusal; normalize by calls and inspect unaffected short waits. See [refusal analysis](../experiments/corpus/sleep-refusal.scm) and [agent trials](agents.md). | The instruction and advice did not stop long fixed waits. After the refusal, requests for waits at or above 10 seconds fell from 32 to 2 across the historical 20,774-call comparison; short waits stayed at 15 in each window. | Before/after evidence is not randomized. The hook records requests before deciding; the result demonstrates fewer *requests*, not elapsed time saved on each request. |
| Did agents adopt an event wait for local files? | Three trials per condition with neither intervention, hook advice only, or an agent instruction; see [agent trials](agents.md). | The instruction condition used `wait-for` in 3/3 trials with no polling; the hook-only condition used it after polling first in 3/3 trials; neither intervention yielded polling in 3/3 trials. | Small controlled task; does not establish adoption across other tasks or sessions. |
| Did a task-shaped inspector help? | Ask one fixed source-location question with search plus ranged read, then with `inspect`; require the same path, line, and matching source. Run [comparison](../experiments/task-inspect/compare.scm) and [timing](../experiments/task-inspect/bench.scm). | The fixture takes one call instead of two and returns 717 rather than 764 bytes, a 6.2% reduction. Two 300-run blocks in each order were approximately 0.55 ms per task for either path. | A tiny fixture; fewer calls and bytes have not been shown to improve real task completion. The in-process timing excludes MCP transport and model time. |

The original loop did not achieve its central replacement goal. Shell calls
compose operations, often batching reads and transformations in one invocation;
one-operation MCP tools can increase round trips. Byte-identical substitution
cannot reduce bytes, and a fresh interpreter can erase a small in-process speed
gain. The [analysis and cost accounting](relevance.md) sets out those constraints
and the incidents that helped reveal them. Passing `make check`, including
interpreter tests, sanitizers, fuzzing, and performance targets, establishes
implementation health; it does not establish agent efficacy.

The [historical cost ledger](relevance.md) estimated 23 ms of two-hook overhead
per call and 58 ms less requested fixed-wait time per call after refusal, or
35 ms net saved per call in that window. This was an estimate from requests and
changed behavior, not a direct clock measurement of a matched no-hook session.
It also predates the current single-hook Codex setup. The distinction prevents
the older positive ledger and the current negative observation from being
treated as contradictory measurements of one unchanged system.

## The local content-cache premise

There **is** a [content cache](../toolscheme_posix.cpp) for `read-file`: a
64 MB per-process store of raw file contents, admitted on the second read of a
path. A hit still performs `stat` and compares device, inode, size, and
nanosecond modification time; a file modified in the last two seconds is read
again to avoid timestamp-resolution races. The cache can be disabled with
`TOOLSCHEME_CACHE=0`. The [tests](../test_toolscheme.cpp) cover admission and
same-length rewrites. A short-lived CLI or hook process loses the cache at exit;
a persistent MCP server can keep it across calls.

This is not the proposed file-hierarchy index or a parsed/searchable cached file
form. [Repository search](../toolscheme_posix.cpp) walks directories and reads
each selected file on every call; `glob`/`find` also walk directories afresh.
Tagged-source `grep` calls `read-file` and can use its raw-content cache, but it
still expands a glob and processes the lines. The `inotify` code in the same
source is attached to `wait-for`, not to cache invalidation. No persistent
directory index or integration with an external indexed search engine was found
in this implementation. External writes are detected by the fingerprint check
on `read-file`, subject to the documented same-fingerprint edge case, rather
than by a watcher. These distinctions matter more than calling the whole
project simply "cached" or "uncached."

The [local search benchmark](../experiments/local-search/measure.sh) ran 100
launched calls of each command on this checkout, with warm OS caches and output
sent to `/dev/null`. All three content searches found two matches; both file
enumerators found eight C++ files. Approximate time per launched call:

| Operation | Mean on this host |
|---|---:|
| recursive `grep` | 1.9 ms |
| `rg` | 3.8 ms |
| ToolScheme `search` CLI | 13.7 ms |
| `find` | 3.3 ms |
| ToolScheme `glob` CLI | 13.1 ms |

The commands have similar scope but not identical ignore and output semantics;
this is a small-tree startup-and-scan comparison, not a universal ranking.
The [in-process benchmark](../experiments/local-search/in-process.scm) removes
CLI startup: 100 `search` calls took 139-147 ms, and 100 `glob` calls took
25-28 ms. Repeating a full `read-file` of the stable 320 KB `toolscheme.cpp`
3,000 times took 53-54 ms with the cache and 100-101 ms with it disabled:
roughly **1.9 times faster for that read**, but only about **16 microseconds
saved per call**. Search and glob times were effectively unchanged by the
cache switch, consistent with their implementation. These timings exclude MCP
transport, model time, and any cost of ensuring adoption.

Thus the raw-content cache was implemented and works for repeated reads inside
one process. The larger premise -- an external-change-aware, persistent indexed
representation that makes general repository search materially faster -- was
not implemented or demonstrated. On this checkout, common `find`/`grep`/`rg`
operations took a few milliseconds, leaving little room for a large end-to-end
gain even if an index reduced their scan time.

## This host on 2026-10-25

The installed binary and library match this checkout. At measurement time,
Codex had one global ToolScheme `PreToolUse` hook, and the configuration enabled
fixed-wait and local-poll refusals, advice, classification, and repository
dogfooding. The
current observation log contains **6,591 Codex pre-use records**, no post-use
records, no requests for waits of 10 seconds or more, and no commands matching
the exact local-file polling loop. It contains one recognized one-second sleep.
The log has no usable output-byte or completion records for this window. The
historical two-hook cost estimate in [relevance.md](relevance.md) is therefore
not the current Codex configuration.

To estimate current cost, 100 ordinary `PreToolUse` requests were sent through
the installed `observe.sh`, with the real host config and a temporary copy of the
then-current 4.9 to 5.1 MB observation log. The [measurement script](../experiments/host-hook-cost/measure.sh)
preserves the exact input and control loop. Two 100-call runs took **1.67 s** and
**1.61 s** through the hook. An otherwise identical `printf | true` loop took
**0.05 s** in each run: roughly **15.6 to 16.2 ms of incremental hook cost per
call**, or **103 to 107 s across 6,591 calls** if representative. The requests
were isolated from the live log. The temporary
state was removed after the run. This is one local batch, not a latency
distribution or a counterfactual agent trial.

This window shows measurable hook overhead and no *observed* refusal saving.
The absence of long-wait requests could itself be a learned effect of past
refusals or instructions. The log cannot assign causality, and the 757 requests
containing `wait-for` include this investigation's own probes. It would be
incorrect to claim that the hook has never helped on this host, or that removing
it cannot change future behavior. It is fair to say the observed window does
not justify an always-on hook by a measured time saving.

Given that result, the global ToolScheme hook entries were removed from this
host's Codex and Claude Code settings on 2026-10-25. Backups are
`~/.codex/config.toml.before-toolscheme-disable-20261025` and
`~/.claude/settings.json.before-toolscheme-disable-20261025`. Other hooks were
preserved. The ToolScheme CLI, MCP registration, source, config, and observation
log remain available for deliberate use. New agent sessions will read the
changed settings; sessions already running may retain their startup hook
configuration. `make install` configures hooks automatically and would add
them back unless run with `CONFIGURE_HOOKS=0`.

## Interpretation

ToolScheme succeeded as an instrument: it measured the workload, invalidated
its own replacement thesis, and found a specific waste that a refusal changed
in the historical window. Its current inspector demonstrates a small reduction
in calls and bytes, but no established task-level or execution-time gain. The
engineering and host overhead are real. The narrower conclusion is stronger
than calling every part of the project a failure: **the original tool-replacement
claim failed; observation and refusal remain useful research results, with
current host value unproven.**

## If this line of research were revisited

These are conditions for a future study, not a commitment to pursue one:

1. Start with a preregistered outcome such as successful task completion per
   elapsed agent minute. Include model reasoning, MCP transport, tool startup,
   hook cost, and maintenance in the comparison.
2. Use paired real tasks and a control without ToolScheme. Record adoption and
   follow-up reads; fewer calls in a synthetic fixture are insufficient.
3. Compare complete workflows, especially compound shell calls, instead of
   measuring a primitive against one command in isolation.
4. For refusal rules, compare rates of attempted waits, actual elapsed waits,
   task completion, and regressions across matched host/session windows. Keep
   unaffected short waits as a negative control.
5. If a minimal policy hook is the real product, measure it against the current
   interpreter-backed hook and retain only rules whose saved time exceeds their
   measured cost. Recheck the security and parsing behavior of any smaller
   implementation.

No further project work is planned by this document. Preserve the scripts and
negative results so a later researcher can test a different hypothesis without
repeating the same unsupported claim.
