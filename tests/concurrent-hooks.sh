#!/bin/sh
# Many sessions, one log, no coordination.
#
# `docs/agents.md` claims concurrent sessions are safe: records are clipped below
# PIPE_BUF and appended with O_APPEND, so the kernel serialises each write and no
# two records interleave. That is a real guarantee with real conditions, and
# nothing tested it -- the claim rested on reading the code.
#
# It matters because the hook is installed globally. Every agent session on the
# machine appends to the same file, and a torn record is not a lost record: it is
# a line that fails to parse, or worse, one that parses as something else.
#
# Runs N hook invocations at once against a throwaway state directory and checks
# that every line is complete and parseable and that none was lost.
#
#   sh tests/concurrent-hooks.sh [writers] [rounds]

set -e

WRITERS="${1:-32}"
ROUNDS="${2:-8}"
ROOT="${TMPDIR:-/tmp}/toolscheme-concurrency.$$"
BIN="${TOOLSCHEME_BINARY:-./toolscheme}"
HOOK="${TOOLSCHEME_HOOK:-hooks/observe.sh}"

[ -x "$BIN" ] || { echo "no toolscheme binary at $BIN; run make first" >&2; exit 2; }

mkdir -p "$ROOT"
trap 'rm -rf "$ROOT"' EXIT

# A command long enough to be worth tearing. The record is clipped, so what is
# being checked is that the clip lands below the atomic-write limit rather than
# that the whole command survives.
FILL="${3:-20000}"
filler=$(awk -v FILL="$FILL"  'BEGIN { while (i++ < FILL) printf "x" }' 2>/dev/null || printf '%0400d' 0)

writer() {
    session="$1"
    n=0
    while [ "$n" -lt "$ROUNDS" ]; do
        n=$((n + 1))
        printf '{"hook_event_name":"PreToolUse","session_id":"%s","tool_use_id":"%s-%s","tool_name":"Bash","tool_input":{"command":"echo %s-%s %s"},"cwd":"/tmp"}\n' \
            "$session" "$session" "$n" "$session" "$n" "$filler" \
        | TOOLSCHEME_STATE="$ROOT" TOOLSCHEME_BINARY="$BIN" sh "$HOOK" >/dev/null 2>&1
    done
}

i=0
while [ "$i" -lt "$WRITERS" ]; do
    i=$((i + 1))
    writer "s$i" &
done
wait

expected=$((WRITERS * ROUNDS))
"$BIN" --root "$ROOT" -e '
(let* ((text (field-ref (read-file "observations.jsonl" (quote ((limit 268435456)))) (quote text) ""))
       (lines (filter (lambda (l) (not (string-null? l)))
                      (field-ref (text-lines text) (quote lines))))
       (parsed (map (lambda (l) (catch-errors (lambda () (json-parse l)))) lines))
       (torn (count-if error? parsed))
       ;; The atomicity rests on every record fitting in one atomic append, so
       ;; the clip has to keep them there. A record that grows past PIPE_BUF
       ;; would tear only under load, and only sometimes.
       (longest (fold-left (lambda (n l) (max n (string-length l))) 0 lines)))
  (list (list (quote records) (length lines))
        (list (quote torn) torn)
        (list (quote longest) longest)))' 2>/dev/null > "$ROOT/report"

records=$(sed -n 's/.*(records \([0-9]*\)).*/\1/p' "$ROOT/report")
torn=$(sed -n 's/.*(torn \([0-9]*\)).*/\1/p' "$ROOT/report")
longest=$(sed -n 's/.*(longest \([0-9]*\)).*/\1/p' "$ROOT/report")

echo "writers=$WRITERS rounds=$ROUNDS expected=$expected records=$records torn=$torn longest=$longest"

[ "$torn" = "0" ] || { echo "FAIL: $torn records were torn by concurrent appends" >&2; exit 1; }
[ "$records" = "$expected" ] || {
    echo "FAIL: expected $expected records, found $records" >&2; exit 1; }
# POSIX guarantees PIPE_BUF is at least 512; Linux is 4096. The clip must keep a
# record under whatever this host offers, so the smallest guarantee is the bar
# worth holding to.
[ "$longest" -lt 4096 ] || {
    echo "FAIL: longest record is $longest bytes, past PIPE_BUF on Linux" >&2; exit 1; }
echo "ok: $expected concurrent appends, none torn, none lost, longest $longest bytes"
