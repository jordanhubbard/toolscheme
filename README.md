# Toolscheme

Toolscheme is a small C++17 Scheme interpreter for embedding in coding agents. It
favors native byte strings and immutable random-access array lists over standards
certification, and every tool result is structured, canonical, evaluable data
rather than text to be scraped.

## Build And Test

```sh
make test        # 1597 checks, warning-clean at -O3
make sanitize    # the same suite under AddressSanitizer + UndefinedBehaviorSanitizer
make fuzz        # deterministic property fuzzer (seeded; reproduces from its seed)
make bench       # benchmarks with enforced performance targets
make check       # all of the above
```

## Embedding

```cpp
#include "toolscheme.hpp"

toolscheme::Interpreter scheme;
auto result = scheme.eval("(eval (read-from-string \"(+ 20 22)\"))");
```

`Value::list_at` is zero-based for C++ callers. Scheme `list-ref` and `string-ref`
are one-based. Strings store a native `size_t` header followed by bytes and a
trailing NUL; `string_data()` exposes that layout and `as_string()` preserves
embedded NUL bytes. Values remain valid after the interpreter that produced them
is destroyed.

## Generated Code

`read`, `read-from-string`, `write`, `write-to-string`, and `eval` make generated
Scheme a first-class interchange format, within one interpreter or between two:

```scheme
(eval (read-from-string (write-to-string '(+ 20 22))))
; => 42
```

Runtime-bound resources never print as unreadable `#<...>` tokens. A handle writes
as an evaluable, runtime-validated reference; evaluating a stale, revoked, or
foreign one returns a structured error.

## Capabilities

Host operations are explicit. Install a `Capability` under a Scheme name and it
becomes the default for its kind, or pass one as the first argument to override:

```scheme
(pwd)                                   ; uses the installed filesystem capability
(pwd workspace-fs '((output data)))     ; or an explicit one
```

Registration groups install only the primitives an installed capability actually
backs. A missing or denied capability returns a structured error; primitives never
reach the host directly. Reference POSIX adapters for Linux and macOS live in
`toolscheme_posix.cpp` behind a policy (sandbox root, executable allowlist,
environment allowlist, output and timeout limits).

## Tools

Text tools accept inline data *or* a tagged source that names files, so the shapes
agents actually reach for work directly:

```scheme
(grep "needle" '(glob "src/**/*.c") '((limit 20)))   ; grep -rn … | head -20
(grep "needle" '(files "a.txt" "b.txt"))
(grep "needle" '("inline" "lines"))                  ; still pure; no host access
```

Matches carry their own path and line numbers relative to their own file. An
unreadable path becomes a record, not an aborted call.

`git` and `http-request` are typed front ends over the generic process API — they
return records, not porcelain text. Full shells, `awk`, compilers, and package
managers stay delegated to a shell or process capability.

## Stable output

Results are stable by default: host metadata that churns between otherwise
identical calls — modification times, inode numbers, elapsed milliseconds — is
omitted unless requested. Repeating a call yields byte-identical output, which is
what keeps an agent's prompt cache warm.

```scheme
(stat "a.txt")                           ; no volatile metadata
(stat "a.txt" '((volatile #t)))          ; include it
(stat "a.txt" '((fields (path kind size))))
```

Identifiers a caller must act on, such as a job's `pid`, are never stripped.

See `docs/roadmap/active-work.md` for the full contract and
`tests/roadmap-test-map.md` for the coverage map.
