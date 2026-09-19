#!/bin/sh
# Run Codex so that toolscheme can continue it. Use in place of `codex`:
#
#     codex-session.sh "fix the failing tests"
#
# A plain `codex` keeps its agent core in-process, where nothing outside the
# session can reach it -- there is no supported way to hand it a message. Run
# against an app server instead and `codex queue` can, which is what the
# continuation watcher uses. That is the whole reason this wrapper exists, and
# it is why adoption is per-launch rather than a setting.
#
# Everything else is passed straight through, so flags and prompts work as they
# always did.

STATE="${TOOLSCHEME_STATE:-${XDG_STATE_HOME:-$HOME/.local/state}/toolscheme}"
SOCK="${TOOLSCHEME_CODEX_SOCKET:-$STATE/codex.sock}"
mkdir -p "$(dirname "$SOCK")" 2>/dev/null || exit 1

command -v codex >/dev/null 2>&1 || { echo "codex is not on PATH" >&2; exit 1; }

# Reuse a running server; start one otherwise. A socket file left behind by a
# crash would make every later session unreachable while looking fine, so the
# check is for the process, not the file.
if ! pgrep -f "app-server --listen unix://$SOCK" >/dev/null 2>&1; then
  rm -f "$SOCK"
  nohup codex app-server --listen "unix://$SOCK" >"$STATE/codex-app-server.log" 2>&1 </dev/null &
  # The socket appears a moment after the process does; without this the first
  # session races it and falls back to an ordinary unreachable one.
  i=0
  while [ ! -S "$SOCK" ] && [ $i -lt 50 ]; do
    i=$((i + 1))
    sleep 0.2
  done
fi

[ -S "$SOCK" ] || { echo "toolscheme: no codex app server at $SOCK" >&2; exit 1; }

exec codex --remote "unix://$SOCK" "$@"
