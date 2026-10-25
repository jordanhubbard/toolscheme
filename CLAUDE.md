# Working in ToolScheme

For code-location questions, use `toolscheme_inspect` through MCP or run:

```sh
toolscheme -e '(inspect "run_search" (list ".") (quote ((include ("*.cpp")) (context 4) (limit 8))))'
```

It returns matching lines, nearby context, paths, line numbers, and a `truncated`
flag. Narrow the search if truncated. For local file or log conditions, use
`wait-for` rather than a fixed sleep or a one-second polling loop.
