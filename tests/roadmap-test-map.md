# Roadmap Test Map

Every roadmap task must add or update a named test before it can close. Test names use the `task-NN` prefix in `test_toolscheme.cpp` or a dedicated test executable. Capability-backed primitives are also covered by the registry loop: registration, explicit capability dispatch, proper-list result validation, and canonical writing are shared invariants.

| Tasks | Required Test Area |
|---|---|
| 1 | Published semantics agree with executable behavior |
| 2-5 | Value lifetime, scalar allocation, strings, proper/improper lists, random access |
| 6-8 | Reader locations, writer round trips, numeric boundaries, evaluator forms and tail calls |
| 9-10 | Capability denial, proper result validation, handles, jobs, process lifecycle |
| 11-14 | File/path operations, traversal, metadata, glob, tree, search |
| 15-16 | Text transforms, comparison, diff, atomic patching |
| 17-18 | Output, predicates, system/process/terminal callbacks |
| 19-23 | Hash/archive/JSON/editor/shell/platform adapters and denial paths |
| 24 | Registry enumeration and missing-capability behavior |
| 25 | Warning-clean, sanitizer, fuzz, security and performance gates |
| 26 | Documentation snippets compiled or executed by tests |
| 27 | One success and one applicable failure example per registered primitive |
| 28 | Same-runtime and cross-interpreter `read`/`write`/`eval` round trips |
| m0 | Path-mode text tools, working directory, find predicates, sed, stability, git front end |
| m1 | `shell-parse` heredoc/quoting/keyword handling, `platform-facts`, telemetry accounting, published-tool registry |

## Loop coverage (`make loop`)

These need a shell and a network-shaped transport, so they run outside the
hermetic suite.

| Check | Script | Proves |
|---|---|---|
| Log intake | `tests/intake-check.scm` | All four schemas (two Claude, Codex rollouts with their JavaScript-wrapped commands, and the hook) from pasted text, tool names merged across their spellings, cache tokens and the recorded working directory carried through |
| Live observation | `tests/hook-check.scm` | A hook record keeps the directory, call id and timestamp; a failed call files as a completion rather than a second start; a command with no arguments is still distinguishable; a 20k command clips below the atomic-append limit; malformed input never raises |
| Rewrite policy | `tests/redirect-check.scm` | An unclaimed command is left alone; a claimed one is rewritten; a partial pipeline, an uncovered command and a destructive one are all left alone; an unbacked claim is refused; Codex's JavaScript wrapper is preserved around the replacement |
| Proven claims | `tests/proven-check.scm` | Every shape a tool declares proven is recomputed from its recorded cases -- byte-identical output, identical exit status, and faster than the command it replaces -- and a deliberate overclaim is caught |
| Tool library | `tests/tools-check.scm` | Every published tool carries a name, description, typed and documented parameters, a stability contract, and provenance |
| MCP protocol | `tests/mcp-check.scm` | Handshake, tool listing, a derived JSON Schema that is an object rather than an array of pairs, `isError` on failure, parse and method errors, notifications answered with silence |
| Synthesis request | `tests/synthesis-check.scm` | Cache breakpoint after the stable prefix, volatile report after it, structured output requested, valid JSON body |
| Synthesis responses | `tests/synthesis-check.scm` | Refusal, malformed response, and success branches each return a structured result rather than raising |
| Replay safety | `tests/replay-check.scm` | A destructive command is never offered for replay, and a safe shape does not launder the unsafe command line it appeared in |
| Publication gate | `tests/replay-check.scm` | A real fused tool publishes; a lossy candidate that is stabler *and* cheaper is refused, with the disagreeing line as evidence |
| Live loop | `tests/synthesize-live.scm` | Transcripts → opportunity → model → replay → published file. Needs a gateway credential, so it is run by hand with `make synthesize` rather than in the gate |
