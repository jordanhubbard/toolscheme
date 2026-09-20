#!/bin/sh
# PreToolUse / PostToolUse hook, for Claude Code and Codex alike: both send the
# same JSON fields on stdin and accept the same decision JSON on stdout.
#
# Records the call, and prints a rewrite decision only when one is warranted;
# silence means "leave this call alone".
#
# Everything here is defensive on purpose: a hook runs on every single tool call,
# and one that fails or hangs breaks the session it is supposed to be measuring.
# It exits 0 unconditionally and does nothing at all when it cannot find its parts.

# Where this installation lives: the directory above this script, whether that is
# a source checkout or $PREFIX/share/toolscheme.
HOME_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd) || exit 0

# The binary sits beside the library in a checkout and in $PREFIX/bin once
# installed; an explicit TOOLSCHEME_BINARY wins over both.
if [ -n "$TOOLSCHEME_BINARY" ] && [ -x "$TOOLSCHEME_BINARY" ]; then
  BIN="$TOOLSCHEME_BINARY"
elif [ -x "$HOME_DIR/toolscheme" ]; then
  BIN="$HOME_DIR/toolscheme"
elif [ -x "$HOME_DIR/../../bin/toolscheme" ]; then
  BIN=$(CDPATH= cd -- "$HOME_DIR/../../bin" && pwd)/toolscheme
else
  BIN=$(command -v toolscheme 2>/dev/null) || exit 0
fi
[ -n "$BIN" ] && [ -x "$BIN" ] || exit 0
[ -f "$HOME_DIR/hooks/observe.scm" ] || exit 0

# Observations from every session land in one place, outside any repository: the
# hook watches all projects and must be able to write to none of them. That
# directory is also the sandbox root, so this is the only thing it can touch.
STATE="${TOOLSCHEME_STATE:-${XDG_STATE_HOME:-$HOME/.local/state}/toolscheme}"
mkdir -p "$STATE" 2>/dev/null || exit 0

# Classifying a stall needs an HTTPS call, which here means curl, which means the
# process capability. It is granted only when classification is switched on, so
# the ordinary observing hook keeps no ability to run anything at all. The URL
# comes from the environment and never from the transcript being judged.
NET=""
case "$(. "$STATE/config" 2>/dev/null; echo "${TOOLSCHEME_CLASSIFY:-0}")" in
  1|true|yes|on) NET="--allow-process --allow-program curl" ;;
esac
[ "${TOOLSCHEME_CLASSIFY:-}" = "1" ] && NET="--allow-process --allow-program curl"

TOOLSCHEME_BINARY="$BIN" \
TOOLSCHEME_HOOKS="$HOME_DIR/hooks" \
TOOLSCHEME_LIB="$HOME_DIR/lib" \
"$BIN" "$HOME_DIR/hooks/observe.scm" \
  --root "$STATE" \
  --lib "$HOME_DIR/lib" \
  $NET \
  --stdin --text --quiet 2>/dev/null

exit 0
