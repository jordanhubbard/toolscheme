#!/bin/sh
# One pass over every Codex session on this machine, continuing the ones that
# stopped after naming their own next step. Safe to run on a timer.
#
# Codex has no turn-end hook to decline, so unlike the Claude Code path this is
# a poll: it reads the rollout transcripts Codex already writes, and injects
# through `codex queue`, which only reaches sessions launched against the app
# server socket (see hooks/codex-session.sh).
#
# Does nothing at all unless TOOLSCHEME_CODEX_CONTINUE is on. Exits 0 whatever
# happens: a watcher that fails loudly on a timer is worse than one that waits.

HOME_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd) || exit 0

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
[ -f "$HOME_DIR/hooks/codex-continue.scm" ] || exit 0

SESSIONS="${CODEX_HOME:-$HOME/.codex}/sessions"
[ -d "$SESSIONS" ] || exit 0

STATE="${TOOLSCHEME_STATE:-${XDG_STATE_HOME:-$HOME/.local/state}/toolscheme}"

# Same split as observe.sh: settings under XDG_CONFIG_HOME, state where it is.
CONFIG="${TOOLSCHEME_CONFIG:-}"
if [ -z "$CONFIG" ]; then
  for candidate in "${XDG_CONFIG_HOME:-$HOME/.config}/toolscheme/config" "$STATE/config"; do
    [ -r "$candidate" ] && { CONFIG="$candidate"; break; }
  done
fi
if [ -n "$CONFIG" ] && [ -r "$CONFIG" ]; then
  set -a
  . "$CONFIG"
  set +a
fi

# Must match codex-shim.sh, which owns this path and keeps the directory at 0700
# because Codex 0.156 refuses to bind a socket any other user could replace.
TOOLSCHEME_CODEX_SOCKET="${TOOLSCHEME_CODEX_SOCKET:-$STATE/run/codex.sock}"
export TOOLSCHEME_CODEX_SOCKET

# Rooted at the transcripts, which is the only thing it reads; `codex` is the
# only program it may run, and queueing a message is all it does with it.
TOOLSCHEME_LIB="$HOME_DIR/lib" \
"$BIN" "$HOME_DIR/hooks/codex-continue.scm" \
  --root "$SESSIONS" \
  --lib "$HOME_DIR/lib" \
  --allow-process --allow-program codex --allow-program curl \
  --text 2>/dev/null

exit 0
