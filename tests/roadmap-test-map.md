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
