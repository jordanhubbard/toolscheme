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

# Find codex wherever it has moved to.
#
# A systemd user unit inherits no login shell, so its PATH is whatever the unit
# file names -- and codex does not stay put. It was an npm global under
# /usr/bin, then a Homebrew install under /home/linuxbrew/.linuxbrew/bin, and the
# watcher spent nine attempts reporting "could not start codex" because the
# directory it had been told about no longer held one. The attempt bound stopped
# the retrying, and the failure records made it visible, but neither made it
# work.
#
# So the directory is discovered rather than declared. The candidates are the
# usual install prefixes; PATH is consulted first in case this ran from a shell
# that already knows.
for candidate in \
    "$(command -v codex 2>/dev/null)" \
    "$HOME/.local/bin/codex" \
    /home/linuxbrew/.linuxbrew/bin/codex \
    /opt/homebrew/bin/codex \
    /usr/local/bin/codex \
    /usr/bin/codex \
    /bin/codex
do
    [ -n "$candidate" ] && [ -x "$candidate" ] || continue
    PATH="$(dirname "$candidate"):$PATH"
    export PATH
    break
done

# Three stages, because there is one filesystem root per run and this needs two:
# the rollouts, which it reads, and the state directory, which holds the ledger
# the cap is counted from. Folding them together by widening the root to $HOME
# would hand a timer-driven job that already holds process privileges the run of
# the home directory, which is the wrong trade.
#
# 1. How many times each thread has already been continued. State directory,
#    no process access.
USED=$(TOOLSCHEME_LIB="$HOME_DIR/lib" \
"$BIN" "$HOME_DIR/hooks/codex-used.scm" \
  --root "$STATE" \
  --lib "$HOME_DIR/lib" \
  --text 2>/dev/null)

# 2. Decide and queue. Rooted at the transcripts, which is the only thing it
#    reads; `codex` is the only program it may run, and queueing a message is
#    all it does with it.
REPORT=$(TOOLSCHEME_CODEX_USED="$USED" TOOLSCHEME_LIB="$HOME_DIR/lib" \
"$BIN" "$HOME_DIR/hooks/codex-continue.scm" \
  --root "$SESSIONS" \
  --lib "$HOME_DIR/lib" \
  --allow-process --allow-program codex --allow-program curl \
  --text 2>/dev/null)

echo "$REPORT"

# 3. Record every attempt, delivered or not. A failure that leaves no trace is
#    what made this unbounded: the next tick saw a thread that had never been
#    continued and tried again, 431 times on one thread. No process privileges
#    here; it only appends a line.
case "$REPORT" in
  *'"queued":"sent"'*|*'"queued":"failed"'*)
    TOOLSCHEME_CODEX_REPORT="$REPORT" TOOLSCHEME_LIB="$HOME_DIR/lib" \
    "$BIN" "$HOME_DIR/hooks/codex-record.scm" \
      --root "$STATE" \
      --lib "$HOME_DIR/lib" \
      --text >/dev/null 2>&1
    ;;
esac

exit 0

exit 0
