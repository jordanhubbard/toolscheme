# Toolscheme API

Roadmap items 1 and 26. This is the contract an embedder and a generated tool can
both rely on: what a result looks like, what a capability is allowed to do, and
what "stable output" means precisely enough to test.

## Values

| Scheme | C++ | Notes |
|---|---|---|
| `42` | `Value::integer` | 64-bit signed; overflow is an error, not a wrap |
| `42.` | `Value::real` | always written with a period, so a float never reads back as an integer |
| `"text"` | `Value::string` | native bytes; embedded NUL survives a round trip |
| `name` | `Value::symbol` | interned process-wide, so `eq?` is a pointer compare |
| `(a b)` | `Value::list` | contiguous segments: O(1) `list_size`, `list_at`, `cdr` |
| `#!unspecified` | `Value::unspecified` | self-evaluating, so every value has a written form |

Indexing differs by audience on purpose: `Value::list_at` is zero-based for C++
callers, while Scheme `list-ref` and `string-ref` are one-based. Only `#f` is
false — `0` and `'()` are true.

Every value's written form reads back to an equal value. That is not decoration:
generated Scheme is the interchange format between an agent and this runtime, so
`(eval (read-from-string (write-to-string x)))` has to be total. Runtime-bound
resources are the interesting case — a handle writes as an evaluable
`(handle-ref (quote (runtime "...")) ...)` reference that validates on evaluation
and returns a structured error when it is stale, revoked, or from another runtime.

## Result contract

Every utility primitive returns a **proper list of `(name value)` fields** — never
text to be scraped, never an unreadable token.

```scheme
(stat "a.txt")      ; => ((path "a.txt") (kind file) (size 17))
(stat "missing")    ; => ((error "no such file") (code not-found) (operation stat))
```

A failure is a value, not an exception. Errors carry `error` (a string), `code` (a
symbol), and `operation` (a symbol); `denied` and `unsupported` are codes, not
separate shapes. Raised `toolscheme::Error` is reserved for *programming* errors —
wrong arity, wrong type — which a generated tool should never rely on catching.

The test suite enforces this: `tests/primitive_examples.inc` carries a worked
success and failure expression for all 319 primitives, and the build fails if a
registered primitive has no row or a row names a primitive that is not registered.

## Output options

Every utility accepts a trailing options record.

| Option | Effect |
|---|---|
| `(limit N)` | bound the result; no caller should ever need `\| head` |
| `(fields (a b c))` | project to exactly these fields |
| `(volatile #t)` | include host metadata that is omitted by default |
| `(output data\|text\|json)` | render the same structured result differently |

### Stability

Repeating an identical call produces byte-identical output. This is the property
the whole design rests on: unstable output invalidates an agent's prompt-cache
prefix, and re-reading that prefix costs more than the call saved.

Enforced centrally in `apply_output()`, which strips host metadata that churns
between otherwise identical calls — modification and access times, inode numbers,
elapsed milliseconds — unless `(volatile #t)` asks for it.

The line is drawn by *what the caller must act on*, not by what changes. A job's
`pid` changes every run and is never stripped, because a caller that cannot see it
cannot signal the job. An mtime changes every run and is always stripped, because
nothing downstream needs it. When in doubt, ask whether omitting the field breaks
a caller; if it does, it stays.

## Text sources

Text tools take inline data *or* a tagged source naming files, because that is how
agents actually call them — `grep -rn pattern src/`, not on a string they already
hold.

```scheme
(grep "needle" '(glob "src/**/*.c") '((limit 20)))
(grep "needle" '(files "a.txt" "b.txt"))
(grep "needle" '("inline" "lines"))            ; pure; no host access
```

Matches carry their own path and line numbers relative to their own file. An
unreadable path becomes a record in the result, not an aborted call.

## Capabilities

Host operations are reachable only through a capability. A primitive validates and
types its arguments, dispatches exactly one operation through
`dispatch_capability()`, and validates that the host returned a proper list.
Capabilities never see Scheme source; primitives never touch the host.

There are 16 kinds: `filesystem`, `process`, `shell`, `terminal`, `clock`,
`system`, `service`, `archive`, `compression`, `crypto`, `network`, `http`,
`remote-shell`, `editor`, `logging`, `desktop`.

```scheme
(pwd)                                   ; the installed filesystem capability
(pwd workspace-fs '((output data)))     ; or an explicit one, passed first
```

Registration groups install only the primitives an installed capability actually
backs, so a runtime with no process capability has no `git` primitive to deny —
the operation is absent rather than present-and-refused. A denied operation
returns `(code denied)`; an unavailable one returns `(code unsupported)`.

### Policy

The reference POSIX adapters (`toolscheme_posix.cpp`) take a `Policy`:

| Field | Meaning |
|---|---|
| `root` | every path resolves inside this directory; traversal and symlink escapes are rejected before any host call |
| `writable` | refuse mutating operations |
| `allow_process`, `allowed_programs` | child processes are refused unless enabled; an empty allowlist with `allow_process` permits any program on the search path |
| child working directory | `(directory ...)` on a process call is resolved against the root like every other path; absolute paths are accepted and one outside the root is refused |
| `environment_allowlist` | what `env` reports and what children inherit |
| `output_limit`, `default_timeout_ms` | bound every result and every wait |
| `allow_terminal`, `allow_service`, `allow_logging`, `allow_desktop` | off by default |

Installing nothing is a supported configuration: the interpreter runs with every
host operation absent, which is the right posture for evaluating code a model just
wrote.

## Telemetry

Off by default. When enabled, every capability call is recorded at
`dispatch_capability()` — the single point all of them pass through, so a call
cannot be made without being counted.

```scheme
(telemetry 'start)
(telemetry 'summary)   ; => ((tools (((tool "stat") (calls 3) (total-ms 0) (bytes 210) (errors 1)) ...)) ...)
(telemetry 'clear)
```

A structured failure counts as a call with an error, not as a missing call.

## Standard input

`--stdin` binds the process's standard input to `standard-input` as a string.
Nothing in the language can reach it otherwise: a script that consumes stdin has to
say so on the command line, so one that does not ask can never block on a pipe.
This is what lets toolscheme serve as its own agent hook, reading the hook's JSON
on stdin at about 2.5 ms per invocation.

## Published tools

A published tool is an ordinary Scheme procedure plus the metadata an agent needs
to discover it and the provenance that says why it exists.

```scheme
(define-tool (list (list 'name "search-read")
                   (list 'description "...")
                   (list 'parameters '((pattern string "...") ...))
                   (list 'stability "...")
                   (list 'provenance '((pattern "grep -> read") (observed-pairs 192)))
                   (list 'procedure search-read)))
```

`(tool-manifest)` lists them and `(tool-invoke name arguments)` calls one. The
manifest deliberately omits the procedure: it describes what may be called, and a
procedure has no transferable written form.

Tools live in `lib/tools/*.scm`, are loaded at startup, and are exported over MCP
alongside a generic `toolscheme_eval`. Publishing is writing a file, so reverting
is deleting one.

Arguments arriving over MCP are JSON, where there are no symbols. A tool that
takes a tagged source should accept the JSON spellings too — `"lib/*.scm"` and
`["glob", "lib/*.scm"]` as well as `'(glob "lib/*.scm")` — rather than requiring
an MCP-shaped duplicate of itself.

## Embedding

```cpp
#include "toolscheme.hpp"

toolscheme::Interpreter scheme;
toolscheme::posix::install_all(scheme, policy);
auto result = scheme.eval("(grep \"needle\" '(glob \"src/**/*.c\"))");
```

Values remain valid after the interpreter that produced them is destroyed.
`collect()` breaks reference cycles among unreachable environments and closures and
returns how many it reclaimed.
