#!/bin/sh
# PreToolUse / PostToolUse observer. Records one tool call and gets out of the way.
#
# Everything here is defensive on purpose: a hook runs on every single tool call,
# and one that fails, blocks, or writes to stdout can break the session it is
# supposed to be measuring. So it exits 0 unconditionally, prints nothing, and
# does nothing at all when the binary has not been built yet.
[ -n "$CLAUDE_PROJECT_DIR" ] || exit 0
[ -x "$CLAUDE_PROJECT_DIR/toolscheme" ] || exit 0

"$CLAUDE_PROJECT_DIR/toolscheme" "$CLAUDE_PROJECT_DIR/hooks/observe.scm" \
  --root "$CLAUDE_PROJECT_DIR" \
  --lib "$CLAUDE_PROJECT_DIR/lib" \
  --stdin --quiet >/dev/null 2>&1

exit 0
