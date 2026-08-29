# Toolscheme

Toolscheme is a small C++17 Scheme interpreter intended for embedding in coding agents. It favors native byte strings and immutable random-access array lists over standards certification.

## Build And Test

```sh
make test
make sanitize
```

## Embedding

```cpp
#include "toolscheme.hpp"

toolscheme::Interpreter scheme;
auto result = scheme.eval("(eval (read-from-string \"(+ 20 22)\"))");
```

`Value::list_at` is zero-based for C++ callers. Scheme `list-ref` and `string-ref` are one-based. Strings store a native `size_t` header followed by bytes and a trailing NUL. `string_data()` exposes that layout and `as_string()` safely preserves embedded NUL bytes.

## Generated Code

`read-from-string`, `write-to-string`, and `eval` support data and code handoff:

```scheme
(eval (read-from-string (write-to-string '(+ 20 22))))
; => 42
```

## Capabilities

Host operations are explicit. Install a `Capability` under a Scheme name, then pass it as the first argument to a utility:

```scheme
(pwd workspace-fs '((output data)))
(rg workspace-fs "needle" '("src") '((literal #t)))
```

The capability receives the operation name and typed arguments. It must return a proper Scheme list. Toolscheme itself does not silently access the host filesystem, process table, terminal, network, or service manager.

Large systems such as Git, compilers, package managers, and full shells remain delegated through process or shell capabilities. See `docs/roadmap/active-work.md` for the complete contract and `tests/roadmap-test-map.md` for required coverage.
