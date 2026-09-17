#!/bin/sh
# Does steering change what an agent does, or only what it is told?
#
# The project has been arguing from a corpus: 9.3 hours of `sleep` out of 52.8
# means agents poll, therefore build a wait primitive and tell them about it. That
# is an argument from observation, and observation cannot distinguish a missing
# tool from a missing instruction. This runs the intervention instead.
#
# The same task, some trials with steering on and some with it off, measured from
# the observation log this project already collects. Each trial gets a fresh state
# directory so one trial's session-start note cannot reach the next.
#
#   usage: tests/steering-experiment.sh [trials]      (default 3 per arm)

set -e
TRIALS="${1:-3}"
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
WORK=${WORK:-/tmp/steer-experiment}
# Two waits, because PreToolUse advice is reactive: it arrives once the agent has
# already chosen to sleep, and only a later wait in the same session can show
# whether it landed.
TASK='Do this in order, in the current directory. 1) Run: (sleep 25; touch ONE) &
then wait until ONE exists. 2) Run: (sleep 25; touch TWO) & then wait until TWO
exists. 3) Say how you waited each time.'

rm -rf "$WORK"; mkdir -p "$WORK"

for arm in off on; do
  for i in $(seq 1 "$TRIALS"); do
    state="$WORK/state-$arm-$i"
    cwd="$WORK/run-$arm-$i"
    mkdir -p "$state" "$cwd"
    printf 'TOOLSCHEME_STEER=%s\n' "$([ "$arm" = on ] && echo 1 || echo 0)" > "$state/config"
    TOOLSCHEME_STATE="$state" timeout 300 codex exec \
        --sandbox danger-full-access --skip-git-repo-check --cd "$cwd" \
        "$TASK" < /dev/null > "$WORK/log-$arm-$i.txt" 2>&1 || true
    echo "  $arm trial $i done"
  done
done

# An A/B whose treatment was never delivered produces a clean null result that
# means nothing. Codex silently skips untrusted hooks, so this asserts the
# intervention actually arrived before any comparison is worth reading.
delivered=0
for i in $(seq 1 "$TRIALS"); do
  if grep -qs 'SESSION-ADVICE\|ADVISED-SLEEP' "$WORK/state-on-$i/sessions/"*.keys; then
    delivered=$((delivered + 1))
  fi
done
# A control that received the treatment by another route is not a control. The
# note also lives in AGENTS.md and CLAUDE.md, which every session loads regardless
# of this flag, so the flag gates only the per-call hook advice. If the control arm
# already behaves, that channel is doing the work and the hook's marginal effect
# cannot be read from these numbers.
adopted_off=0
for i in $(seq 1 "$TRIALS"); do
  if grep -qs 'wait-for' "$WORK/state-off-$i/observations.jsonl"; then
    adopted_off=$((adopted_off + 1))
  fi
done

echo "runs complete: $WORK"
echo "treatment delivered in $delivered of $TRIALS 'on' trials"
echo "control arm already using the tool in $adopted_off of $TRIALS trials"
if [ "$adopted_off" -gt 0 ]; then
  echo "CONTROL NOT CLEAN: the instruction reached the control arm through"
  echo "AGENTS.md or CLAUDE.md, which no flag here gates. Whatever the arms show,"
  echo "it is not the hook's marginal effect. Remove that section, or run the"
  echo "control under a CODEX_HOME that does not carry it."
fi
if [ "$delivered" -eq 0 ]; then
  echo "NOT DELIVERED: no advice reached any treatment trial, so the arms are not"
  echo "comparable. Most likely the hook is not trusted -- an untrusted hook is"
  echo "skipped in silence -- or the task never induced the behaviour it targets."
  exit 3
fi
