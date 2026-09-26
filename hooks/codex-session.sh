#!/bin/sh
# Run Codex so that toolscheme can continue it. Use in place of `codex`:
#
#     codex-session.sh "fix the failing tests"
#
# A plain `codex` keeps its agent core in-process, where nothing outside the
# session can reach it -- there is no supported way to hand it a message. Run
# against an app server instead and `codex queue` can, which is what the
# continuation watcher uses.
#
# The mechanism itself now lives in codex-shim.sh, which is the same script
# installed as `codex` so that nothing has to be typed differently. This remains
# the explicit opt-in for anyone who would rather not have a shim on PATH, and
# it is a wrapper rather than a copy: the socket path was duplicated across
# three files, and when Codex 0.156 began rejecting group-writable socket
# directories, fixing one of the three left the other two silently broken.
TOOLSCHEME_CODEX_CONTINUE=1
export TOOLSCHEME_CODEX_CONTINUE
exec "$(dirname "$0")/codex-shim.sh" "$@"
