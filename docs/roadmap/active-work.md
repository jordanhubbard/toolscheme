# Toolscheme Active Work Roadmap

## Mission

Build a tiny, high-performance, embeddable Scheme interpreter for coding agents. It is intentionally not standards-certified. It should represent native values efficiently, expose aggregate values through random-access APIs, and provide the small operating-system and text operations that coding CLIs such as Claude Code and Codex routinely need.

The production interpreter remains centered on `toolscheme.cpp` and `toolscheme.hpp`. Tests, benchmarks, and optional platform or library adapters may live in separate files.

## Scope Principles

- Implement small, portable, frequently used coding-agent operations as typed Scheme primitives.
- Return proper random-access lists from every utility primitive, including failures.
- Require explicit host capabilities for filesystem, process, environment, terminal, clock, service, archive, compression, crypto, and network access.
- Delegate languages, compilers, package managers, version-control systems, build systems, debuggers, full shells, and large interactive applications to capability-controlled external processes.
- Keep toolscheme permissively licensed. Full shell compatibility is supplied by host callbacks rather than embedded GPL shell implementations.
- Support macOS, Linux, and a portable POSIX core. Platform-specific operations return structured unsupported-platform results.
- Prefer typed arguments and structured results over command-line token parsing and formatted terminal output.
- Require every primitive result to have a canonical, proper, readable Scheme representation that can be parsed and evaluated again.
- Require every primitive's behavior, options, limits, and capability references to be expressible through its own explicit arguments rather than hidden command-line or process-global state.

## Language And Storage Contract

- Integers are native signed 64-bit values written without a suffix, for example `42`.
- Floats are native doubles identified by a trailing period, for example `42.`. The writer and lexer must define a round-trippable form for fractional doubles.
- Integer arithmetic is checked; overflow returns a precise error rather than invoking C++ undefined behavior.
- Strings are immutable UTF-8 byte sequences. A native `size_t` header stores the byte length, followed by bytes and a host-friendly NUL terminator.
- String indexing is 1-based and counts UTF-8 bytes, matching the requested `str[1..n]` layout.
- Proper lists are immutable contiguous arrays with O(1) length, indexing, `car`, and `cdr` views. This deliberately favors array semantics over traditional pair identity.
- `cons` onto a proper list normalizes into proper contiguous storage and may cost O(n). Bulk builders avoid repeated `cons` when constructing large lists.
- Improper pairs remain representable separately.
- Returned values remain valid after their originating interpreter is destroyed.
- Only `#f` is false. Nil, booleans, integers, floats, characters, and unspecified values should be immediate values without per-value allocation.

## Read, Write, And Eval Contract

Toolscheme must support generated Scheme as a first-class interchange format. One interpreter or command must be able to emit canonical Scheme source that another interpreter can read and evaluate directly.

```scheme
(write-to-string '(+ 1 2))
; => "(+ 1 2)"

(eval (read-from-string "(+ 1 2)"))
; => 3
```

Required primitives:

```text
read read-from-string write write-to-string eval
```

Rules:

- `read` and `read-from-string` return Scheme data without evaluating it.
- `write` and `write-to-string` produce canonical, escaped, round-trippable Scheme source rather than diagnostic text.
- `eval` accepts parsed Scheme data and an optional explicit environment argument. With no environment argument it uses a documented interaction environment.
- Source emitted by `write` must satisfy `equal?` after a write/read round trip for every readable value.
- Procedures, active jobs, file handles, sessions, terminals, and other runtime-bound opaque resources are not silently printed as unreadable `#<...>` tokens. A primitive returning such a resource must emit an evaluable reference form, such as a runtime-validated handle lookup expression, or return a structured non-serializable error when transfer is impossible.
- Serialized capability and handle references remain unforgeable, scoped, revocable, and runtime-validated. Evaluating a stale or foreign reference returns a proper structured error list.
- Every primitive returns a proper list whose canonical written form is valid Scheme source and can be evaluated to reconstruct the result data or a validated reference to its runtime-bound resource.
- Every primitive accepts all behavior-controlling values through explicit Scheme arguments, including operation, options, limits, input, destination, environment, capability, and async mode where applicable.
- Every primitive can permute its supported behavior through typed option arguments, including selecting canonical data, evaluable source, or an evaluable expression result when those output modes are meaningful. Output-mode selection never changes the proper-list result envelope.
- Primitive defaults are documented and expand to the same behavior as their explicit argument form; no primitive depends on ambient process arguments, implicit standard streams, process-global working directory, or unrestricted host state.
- Generated-code handoff must work both within one interpreter and between independent interpreter instances when the result contains only transferable data.

For example, a primitive may expose explicit output behavior without introducing a separate textual CLI:

```scheme
(pwd '((output data)))
; => ((path "/workspace"))

(pwd '((output source)))
; => ((source "((path \"/workspace\"))"))

(eval (read-from-string
       (list-ref (list-ref (pwd '((output source))) 1) 2)))
; => ((path "/workspace"))
```

The exact option record may be specialized by primitive, but every supported permutation must be discoverable, typed, serializable, and supplied through arguments.

## Utility Result Contract

Every utility returns a proper list. Results are tool-specific but use common association-list conventions. The complete result must have a canonical written representation that is valid, evaluable Scheme.

```scheme
(pwd)
; => ((path "/workspace"))

(cat '("README.md") '())
; => ((data (#\# #\space #\T ...))
;     (files (((path "README.md") (size 128)))))

(rm '("/denied") '())
; => ((error "permission denied")
;     (code permission-denied)
;     (operation rm)
;     (path "/denied"))

(sleep 2.)
; => ((job #<job:1>) (state running))
```

Opaque jobs, files, directory streams, terminals, editor sessions, and shell sessions are unforgeable values wrapped in result lists. Handles carry runtime identity, capability identity, resource kind, generation, and a host-owned resource token. Their written form must be an evaluable, runtime-validated reference expression rather than unreadable diagnostic syntax.

## Capability Architecture

Define optional callback interfaces in the public header:

```text
FileSystemCapability
ProcessCapability
ShellCapability
TerminalCapability
ClockCapability
SystemCapability
ServiceCapability
ArchiveCapability
CompressionCapability
CryptoCapability
NetworkCapability
HttpCapability
RemoteShellCapability
EditorCapability
LoggingCapability
DesktopCapability
```

Rules:

- Missing or denied capabilities return structured error lists.
- Primitives never bypass callbacks with direct host access.
- The filesystem capability owns the interpreter-visible working directory.
- Process capabilities control executable allowlists, search paths, environment, working directories, resource limits, network inheritance, terminals, timeouts, output limits, signals, and child creation.
- Network access is not implied by filesystem or process access.
- Host adapters may use POSIX APIs, optional permissively licensed libraries, installed executables, remote services, or explicit denial.

## Native Coding-Agent Primitives

### Files And Paths

```text
basename cat chmod cp dd df dirname du file find link ln ls mkdir mktemp
mv pwd readlink realpath rm rmdir stat sync touch truncate unlink
```

Also provide typed handle operations:

```text
file-open file-close file-read file-read-at file-write file-write-at
file-seek file-stat file-truncate file-flush
```

Path operations must defend against traversal, symlink races, recursive-link surprises, embedded NULs, and capability-root escape.

### Repository Inspection

```text
glob tree read-file search rg apply-patch temp-file temp-directory
```

- `glob` returns sorted structured path records with hidden-file, ignore, type, and traversal controls.
- `tree` returns bounded structured directory trees.
- `read-file` supports byte ranges, line ranges, output limits, and binary policy.
- `search` and `rg` provide literal or regular-expression search with include/exclude globs, context, limits, and path/line/column/offset records.
- `apply-patch` supports atomic unified-diff add, update, and delete operations with expected old content, dry runs, path policy, and rejection records.

### Text Processing

```text
comm cut diff diff3 expand fmt fold grep head join nl paste rev sort split
strings tail tr unexpand uniq wc xargs
```

Provide typed transformations in place of embedded text languages:

```text
text-lines text-fields text-select text-replace
```

Do not natively reimplement `awk`, `sed`, Perl, or another programmable text language.

### Inspection, Hashing, And Encoding

```text
cksum cmp md5 shasum sum which whereis base64 hash
```

Digest results include algorithm, raw byte list, and canonical hexadecimal string. Available algorithms are controlled by `CryptoCapability`.

### Processes And System Information

```text
date env hostname id kill nice pgrep pkill ps sleep time timeout tty uname
uptime users wait4path who whoami
```

Environment and system identity are capability-filtered. Potentially blocking operations return jobs.

### Output And Predicates

```text
[ echo expr printf test yes
```

`[` and `test` share a typed predicate implementation. `expr` uses typed arithmetic, comparison, boolean, string, and filesystem predicates. `printf` formats values but performs no shell evaluation.

### Archives And Compression

```text
bzip2 compress cpio gunzip gzip pax tar uncompress unzip zip
```

These are typed front ends over optional archive and compression capabilities, preferably backed by permissively licensed libraries such as libarchive. Archive extraction rejects path traversal unless explicitly authorized.

### Terminal And Platform Operations

```text
launchctl logger open stty sync wait4path
```

`launchctl` is Darwin-specific. Linux may expose `systemctl` through the service capability, but platform-specific names retain their actual semantics. `stty` requires an explicit terminal handle and never silently modifies the embedding process terminal.

## Original Requested Tool Manifest

The initial requested names remain explicit deliverables:

```text
[ dd launchctl pwd tcsh
bash df link realpath test
cat echo ln rm unlink
chmod ed ls rmdir wait4path
cp expr mkdir sh zsh
csh hostname mv sleep
dash kill pax stty
date ksh ps sync
```

Small operations use typed native primitives. Shared aliases use shared implementations. Stateful or large tools use capability adapters.

## Delegated Tools

These tools are commonly called by coding agents but are too large, ecosystem-dependent, or language-like to reimplement. They use the generic process or specialist capability APIs.

### Version Control

```text
git hg svn
```

### Build Systems

```text
bazel cmake make meson ninja xcodebuild
```

### Compilers, Linkers, And Toolchains

```text
ar as cc clang clang++ cpp c++ gcc g++ ld nm objdump ranlib size strip
```

### Language Runtimes

```text
bun deno go java javac lua node perl php python python3 ruby rustc swift
```

### Package Managers

```text
brew cargo gem npm npx pip pip3 pnpm yarn
```

### Test And Analysis Tools

```text
ctest eslint mypy pytest ruff shellcheck swiftlint tsc valgrind
```

### Full Shells And Text Languages

```text
awk bash csh dash fish ksh sed sh tcsh zsh
```

The shell names dispatch to `ShellCapability` with explicit dialect identity. The host may supply an installed shell, separately licensed engine, remote executor, or denial. Toolscheme does not embed Bash-derived GPL code.

### Editors, Pagers, And Interactive Applications

```text
ed less more nano vi vim
```

A small typed editor-session API covers common automated edits. Exact editor compatibility remains delegated.

### Network And Remote Tools

```text
curl ftp nc ping scp ssh telnet
```

Typed HTTP can cover common `curl` use, while exact CLI compatibility and SSH remain host-provided.

## Generic Asynchronous Process API

Project-specific tools require a safe escape hatch:

```text
process-start process-poll process-wait process-cancel process-write
process-close-input process-read-output process-read-errors
```

```scheme
(process-start
 '((program "git")
   (arguments ("status" "--short"))
   (directory "/workspace")
   (environment ())
   (stdin ())
   (timeout-ms 30000)
   (output-limit 10485760)))
```

Completion records include state, exit status, signal, stdout, stderr, truncation, PID, and elapsed time.

## Job And Session APIs

Asynchronous jobs:

```text
job-poll job-wait job-cancel job-input job-close-input
job-output job-error-output job-status
```

Stateful editor sessions:

```text
ed-open ed-command ed-buffer ed-write ed-close
```

Stateful shell sessions are host-backed opaque handles. All session operations return lists.

## Interpreter Completion Work

- Replace the prototype allocation-per-value representation with compact tagged immediate values and managed aggregate objects.
- Remove public internal-storage hooks and ambiguous constructors; expose explicit value factories.
- Add shared runtime ownership so returned values outlive an interpreter safely.
- Add cycle-aware collection for closures, environments, aggregate objects, and external roots.
- Repair proper and improper list normalization and provide bulk list builders.
- Add a lexer with source filename, offset, line, and column tracking.
- Parse numeric-looking tokens deterministically and reject unsupported or overflowing forms.
- Implement a readable, escaped, round-trippable writer.
- Add `read`, `read-from-string`, `write`, `write-to-string`, and Scheme-visible `eval` with explicit environment semantics.
- Enforce canonical evaluable output for every primitive, including safe evaluable references for runtime-bound handles.
- Make every primitive's behavior fully selectable through explicit Scheme arguments and document all defaults.
- Add checked integer arithmetic and precision-safe numeric comparisons.
- Complete tail dispatch through procedure bodies, `if`, `begin`, `and`, and `or`.
- Make environment traversal iterative.
- Add `quote`, quasiquote, conditionals, definitions, assignment, lambdas, binding forms, and practical control forms.
- Add practical list, string, character, symbol, type, equality, and higher-order procedures.
- Stream top-level file parsing and evaluation rather than materializing entire files.
- Preserve the small public `toolscheme.hpp` plus single implementation-file design.

## Registration Groups

```text
enable_path_primitives
```

Groups register only primitives supported by installed capabilities. JSON is included because coding agents commonly exchange JSON and it is a data format, not a programming language:

```text
json-parse json-write
```

Objects map to ordered association lists.

## Testing And Performance

- Replace `assert`-only tests with always-enabled expected-value and expected-error helpers.
- Test equivalent proper-list construction through literals, dotted forms, `list`, and `cons`.
- Test empty, large, improper, sliced, and randomly indexed lists.
- Test UTF-8 bytes, embedded NULs, escapes, strings larger than 255 bytes, 1-based indexing, and native length headers.
- Test integer boundaries, overflow, mixed comparisons, values above `2^53`, fractional floats, and writer/parser round trips.
- Test `eval` over data produced by `read`, `read-from-string`, `write`, and `write-to-string` in the same and independent interpreter instances.
- Enumerate every registered primitive and verify its success and error results are proper lists whose canonical output parses and evaluates correctly.
- Verify every primitive's supported behavior and options can be selected solely through explicit arguments, with no dependence on ambient command-line or process-global state.
- Test closures, recursion, mutual recursion, rest arguments, deep environments, and at least one million tail calls.
- Test values and handles that survive interpreter destruction.
- Test every capability-denial path and ensure denied callbacks cannot reach host APIs.
- Test traversal, symlink, archive, output-flooding, cancellation, timeout, malformed-data, and handle-forgery attacks.
- Fuzz the lexer, parser, writer round trips, structured requests, JSON, archive manifests, and patch application.
- Run optimized warning-clean builds plus AddressSanitizer, UndefinedBehaviorSanitizer, and leak checks.
- Benchmark scalar allocation, parsing, list construction, list indexing, `cdr`, strings, glob, search, ranged reads, stat, patching, process launch, and directory traversal.

Performance targets:

- No heap allocation for immediate scalar values.
- One aggregate allocation for a parsed proper list where feasible.
- O(1) proper-list length and random access.
- O(1) `cdr` views.
- Linear-time bulk result-list construction.
- No C++ stack growth during tail recursion.
- No whole-list temporary copy merely to inspect arguments.

## Planned Work Items

1. Freeze and document language, storage, indexing, equality, ownership, and complexity semantics.
2. Replace `Value` with compact immediate scalars and managed aggregate values.
3. Implement shared runtime ownership and cycle-aware garbage collection.
4. Implement native-sized UTF-8 byte strings with 1-based indexing.
5. Implement immutable contiguous random-access lists, improper pairs, views, and bulk builders.
6. Replace the parser with a source-located lexer and streaming parser.
7. Implement checked numeric operations, safe comparisons, and round-trippable writing.
8. Complete evaluator semantics, tail calls, environments, binding forms, and core procedures.
9. Add opaque handles, structured result/error builders, and capability interfaces.
10. Add asynchronous jobs and the generic process execution API.
11. Implement basic path and file-handle operations.
12. Implement bulk and recursive filesystem primitives.
13. Implement glob, tree, ranged reads, temporary resources, and structured metadata.
14. Implement search and `rg`-style structured repository inspection.
15. Implement native text transformations, comparisons, sorting, and counting.
16. Implement structured diff and atomic patch application.
17. Implement output, expression, predicate, date, environment, and system-information primitives.
18. Implement process inspection, signals, timing, path waiting, and terminal operations.
19. Implement hashing, encoding, compression, and archive capability adapters.
20. Implement JSON parsing and serialization.
21. Implement editor session primitives and delegated interactive-tool adapters.
22. Implement host shell dispatch for Bash, POSIX sh, Zsh, csh, tcsh, ksh, and Dash.
23. Implement Darwin, Linux, and portable POSIX service and platform adapters.
24. Add registration groups and capability-denial behavior.
25. Add comprehensive correctness, security, ownership, fuzz, sanitizer, portability, and performance tests.
26. Document the public API, typed primitive signatures, result schemas, capability policy, platform support, and delegated-tool boundary.
27. Add executable functionality tests and working examples for every registered primitive, with registry-completeness enforcement.
28. Implement `read`, `read-from-string`, `write`, `write-to-string`, and Scheme-visible `eval`; enforce proper evaluable output and explicit argument-driven behavior for every primitive.

## Delivery Order

Work proceeds in planned-item order unless an item explicitly becomes independently executable. Foundational representation, ownership, and capability work must land before broad primitive implementation. New primitives must use bulk list construction and must not reintroduce direct host access or embedded large-language behavior.

## Milestone 0 — measured baseline

Grounded in 158 real transcripts rather than guesswork; see
`docs/roadmap/self-improvement.md` for the measurements.

- m0.1 Dual-mode text tools: every text tool accepts inline data or a tagged source naming files.
- m0.2 Fill the measured gaps: `cd`, `find` predicates, `sed` as a typed line editor.
- m0.3 Typed front ends for `git` and `http-request` over the process capability.
- m0.4 Output-stability contract enforced centrally, with a test that identical calls are byte-identical.

## Milestone 1 — the loop, end to end

- m1.1 `toolscheme` binary: `-e`, script execution, `repl`, `mcp`, `analyze`, policy flags.
- m1.2 Telemetry at `dispatch_capability()`, `shell-parse` tokenizer, `platform-facts` probe.
- m1.3 Log intake for both transcript schemas, from a path or pasted text.
- m1.4 Analysis: hot tools, shell-AST command shapes, repeats, output cost, fusion candidates, ranked by recoverable waste.
- m1.5 Synthesis over the HTTP capability with structured output and a cache breakpoint. **The live call is unverified: no API key in the development environment.**
- m1.6 Differential replay gate: equivalence by rendering, cost charged for follow-up reads, publication only when equivalent *and* winning.
- m1.7 Publication: `define-tool`, `tool-manifest`, provenance headers, MCP export.
