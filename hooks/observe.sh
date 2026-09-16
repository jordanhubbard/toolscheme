#!/bin/sh
# PreToolUse / PostToolUse hook, for Claude Code and Codex alike: both send the
# same JSON fields on stdin and accept the same decision JSON on stdout.
#
# Records the call, and prints a rewrite decision only when one is warranted;
# silence means "leave this call alone".
#
# Everything here is defensive on purpose: a hook runs on every single tool call,
# and one that fails or hangs breaks the session it is supposed to be measuring.
# It exits 0 unconditionally and does nothing at all when the binary is missing.

# Locating the project from the script's own path rather than from an environment
# variable, because the two agents do not agree on what that variable is called.
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd) || exit 0
[ -x "$ROOT/toolscheme" ] || exit 0

TOOLSCHEME_BINARY="$ROOT/toolscheme" \
TOOLSCHEME_HOOKS="$ROOT/hooks" \
TOOLSCHEME_LIB="$ROOT/lib" \
"$ROOT/toolscheme" "$ROOT/hooks/observe.scm" \
  --root "$ROOT" \
  --lib "$ROOT/lib" \
  --stdin --text --quiet 2>/dev/null

exit 0
