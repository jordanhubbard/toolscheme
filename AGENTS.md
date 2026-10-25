# ToolScheme in this repository

Use CodeGraph first when `.codegraph/` exists. Otherwise, use ToolScheme's
repository operations for locating code in this tree.

For a code-location question, ask for matching lines and context in one call:

```sh
toolscheme -e '(inspect "run_search" (list ".") (quote ((include ("*.cpp")) (context 4) (limit 8))))'
```

The result includes paths, line numbers, and `truncated`. If truncated is true,
narrow the roots or include pattern before treating the answer as complete. The
same operation is available as the `toolscheme_inspect` MCP tool.

For a local file or log condition, use `wait-for` instead of a fixed sleep or a
one-second polling loop:

```sh
toolscheme -e '(wait-for (quote (exists "path/to/file")))'
toolscheme -e '(wait-for (quote (matches "build.log" "ready")))'
```

Use `(apropos "name")` to discover a binding when unsure of its spelling.
