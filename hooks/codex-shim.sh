#!/bin/sh
# A `codex` that toolscheme can continue, installed earlier on PATH than the real
# one so nothing has to be typed differently.
#
# Codex has no turn-end hook, so the Claude Code approach -- decline the stop --
# has nothing to attach to. What it has instead is an app server: a session
# launched against one can be handed a message by `codex queue` while it sits
# idle. That works, and was verified end to end, but it only reaches sessions
# started with `--remote`, and nobody types `codex-session.sh`. Hence a shim.
#
# Three rules govern it, because a wrapper around someone's main tool is only
# acceptable if it cannot make things worse:
#
#   1. It never fails closed. If the app server will not start, or anything else
#      goes wrong, it execs the real codex unchanged. A continuation feature is
#      not worth breaking the agent it is meant to help.
#   2. It only adds --remote where a session can be continued at all. `exec`,
#      `mcp`, `login` and the rest are passed through untouched; a short
#      non-interactive run has no stall to recover.
#   3. It never execs another copy of itself, by any path.

# TOOLSCHEME_CODEX_SHIM_MARKER -- every copy of this file carries this string so
# that the PATH scan below can tell a shim from the real codex. An earlier
# version resolved the real binary as a hardcoded /bin/codex, which is wrong on
# any machine that installs it elsewhere; the obvious fix, "first codex on PATH
# that is not this script", is not enough either, because running the copy in
# the source tree while an installed copy sits on PATH has each one exec the
# other forever. Identity is not the question. Being a shim is.
looks_like_shim() {
  lines=0
  while [ "$lines" -lt 25 ] && IFS= read -r line; do
    lines=$((lines + 1))
    case "$line" in
      *TOOLSCHEME_CODEX_SHIM_MARKER*) return 0 ;;
    esac
  done < "$1"
  return 1
}

REAL=
saved_ifs=$IFS
IFS=:
for dir in $PATH; do
  [ -n "$dir" ] || dir=.
  candidate="$dir/codex"
  [ -x "$candidate" ] && [ ! -d "$candidate" ] || continue
  looks_like_shim "$candidate" && continue
  REAL=$candidate
  break
done
IFS=$saved_ifs
[ -n "$REAL" ] || { echo "toolscheme: no real codex on PATH" >&2; exit 127; }

# Subcommands that are not a continuable session. Everything else -- no
# subcommand at all, a bare prompt, `resume`, `fork` -- is one.
case "$1" in
  exec|review|login|logout|mcp|plugin|app-server|remote-control|completion|\
  update|doctor|sandbox|debug|apply|queue|archive|delete|migrate-rollouts|\
  unarchive|cloud|exec-server|features|help|agents)
    exec "$REAL" "$@"
    ;;
esac

STATE="${TOOLSCHEME_STATE:-${XDG_STATE_HOME:-$HOME/.local/state}/toolscheme}"
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

# Opt in, like everything else here. Without it this is a pass-through.
case "${TOOLSCHEME_CODEX_CONTINUE:-0}" in
  1|true|yes|on) ;;
  *) exec "$REAL" "$@" ;;
esac

# Codex 0.156 refuses to bind a socket whose directory another user could write,
# so the socket gets its own 0700 directory rather than sharing the state dir --
# which a umask of 002 leaves group-writable, and which failed exactly this way.
SOCK="${TOOLSCHEME_CODEX_SOCKET:-$STATE/run/codex.sock}"
SOCKDIR=$(dirname "$SOCK")
mkdir -p "$SOCKDIR" 2>/dev/null || exec "$REAL" "$@"
chmod 700 "$SOCKDIR" 2>/dev/null || exec "$REAL" "$@"

# Reuse a running server; start one otherwise. A socket file left behind by a
# crash would make every later session unreachable while looking fine, so the
# check is for the process rather than the file.
if ! pgrep -f "app-server --listen unix://$SOCK" >/dev/null 2>&1; then
  rm -f "$SOCK"
  # The server has to outlive this terminal, since the whole point is to reach
  # the session later. `setsid` is the clean way and does not exist on macOS,
  # where the shim would otherwise fail rule 1 quietly: no server, no socket,
  # fall through to a plain session, and a user who is told nothing and gets
  # nothing. `nohup` is the portable second choice.
  if command -v setsid >/dev/null 2>&1; then
    setsid "$REAL" app-server --listen "unix://$SOCK" \
      >"$STATE/codex-app-server.log" 2>&1 </dev/null &
  else
    nohup "$REAL" app-server --listen "unix://$SOCK" \
      >"$STATE/codex-app-server.log" 2>&1 </dev/null &
  fi
  i=0
  while [ ! -S "$SOCK" ] && [ $i -lt 25 ]; do
    i=$((i + 1))
    sleep 0.2
  done
fi

# Still nothing? Then the plain session is the right answer, not an error.
[ -S "$SOCK" ] || exec "$REAL" "$@"

exec "$REAL" --remote "unix://$SOCK" "$@"
