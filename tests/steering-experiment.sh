#!/bin/sh
# Does the machinery matter, or does saying it once where the agent already looks?
#
# The corpus showed agents burning hours on fixed waits, and this project answered
# with a tool, a per-call advice hook, and a rewriting gate. Observation cannot say
# which of those did any work, so this runs the intervention: two channels crossed,
# three trials each.
#
#   note     the one-line instruction in the agent's own AGENTS.md
#   hook     the per-call PreToolUse advice, gated by TOOLSCHEME_STEER
#
# Each arm gets its own CODEX_HOME, so the control genuinely lacks the note --
# the first version of this measured nothing because AGENTS.md is global and
# reached the control too.
#
#   usage: tests/steering-experiment.sh [trials]      (default 3 per arm)

set -e
TRIALS="${1:-3}"
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
HOOK="$HOME/.local/share/toolscheme/hooks/observe.sh"
WORK=${WORK:-/tmp/steer-experiment}
TASK='Do this in order, in the current directory. 1) Run: (sleep 25; touch ONE) &
then wait until ONE exists. 2) Run: (sleep 25; touch TWO) & then wait until TWO
exists. 3) Say how you waited each time.'

NOTE='
## Waiting

`toolscheme` is on PATH and returns the moment a condition holds, rather than after
a fixed delay:

    toolscheme -e '"'"'(wait-for (quote (exists "path/to/file")))'"'"'

It takes `(timeout-ms N)`. Prefer it to a fixed `sleep`, or to a
`while ...; do sleep 1; done` polling loop, whenever the wait has an observable end.
'

rm -rf "$WORK"; mkdir -p "$WORK"

for note in off on; do
  for hook in off on; do
    for i in $(seq 1 "$TRIALS"); do
      arm="note-$note.hook-$hook"
      home="$WORK/home-$arm-$i"; state="$WORK/state-$arm-$i"; cwd="$WORK/run-$arm-$i"
      mkdir -p "$home" "$state" "$cwd"
      ln -sf "$HOME/.codex/auth.json" "$home/auth.json"
      printf 'model = "gpt-6-astra"\napproval_policy = "never"\n\n' > "$home/config.toml"
      printf '[[hooks.PreToolUse]]\nmatcher = "*"\n[[hooks.PreToolUse.hooks]]\ntype = "command"\ncommand = "%s"\n' "$HOOK" >> "$home/config.toml"
      [ "$note" = on ] && printf '%s' "$NOTE" > "$home/AGENTS.md"
      printf 'TOOLSCHEME_STEER=%s\n' "$([ "$hook" = on ] && echo 1 || echo 0)" > "$state/config"

      CODEX_HOME="$home" TOOLSCHEME_STATE="$state" timeout 300 codex exec \
          --sandbox danger-full-access --skip-git-repo-check \
          --dangerously-bypass-hook-trust --cd "$cwd" \
          "$TASK" < /dev/null > "$WORK/log-$arm-$i.txt" 2>&1 || true
      echo "  $arm trial $i"
    done
  done
done

echo
printf '%-22s %-10s %-10s %s\n' arm delivered adopted polled
for note in off on; do
  for hook in off on; do
    arm="note-$note.hook-$hook"; d=0; a=0; p=0
    for i in $(seq 1 "$TRIALS"); do
      grep -qs 'ADVISED-SLEEP' "$WORK/state-$arm-$i/sessions/"*.keys && d=$((d+1))
      grep -qs 'wait-for' "$WORK/state-$arm-$i/observations.jsonl" && a=$((a+1))
      # The behaviour being intervened against: a loop that sleeps and re-checks.
      grep -qs 'do sleep' "$WORK/state-$arm-$i/observations.jsonl" && p=$((p+1))
    done
    printf '%-22s %-10s %-10s %s\n' "$arm" "$d/$TRIALS" "$a/$TRIALS" "$p/$TRIALS"
  done
done
