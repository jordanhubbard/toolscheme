#!/usr/bin/env bash
# Measure installed PreToolUse hook cost against a copy of an observation log.
# Usage: measure.sh HOOK_PATH OBSERVATIONS_PATH CONFIG_PATH
set -euo pipefail

hook_path=${1:?pass installed observe.sh path}
log_path=${2:?pass observations.jsonl path}
config_path=${3:?pass host config path}
count=${COUNT:-100}
state_dir=$(mktemp -d /tmp/toolscheme-hook-cost.XXXXXX)
cleanup() {
  unlink "$state_dir/observations.jsonl" 2>/dev/null || :
  unlink "$state_dir/sessions/hook-cost.keys" 2>/dev/null || :
  rmdir "$state_dir/sessions" 2>/dev/null || :
  rmdir "$state_dir" 2>/dev/null || :
}
trap cleanup EXIT
cp "$log_path" "$state_dir/observations.jsonl"
export TOOLSCHEME_STATE="$state_dir" TOOLSCHEME_CONFIG="$config_path"

echo "calls=$count source_log_bytes=$(stat -c %s "$log_path")"
/usr/bin/time -f 'hook_seconds=%e' bash -c '
  for ((i=1; i<=$2; i++)); do
    printf "{\"hook_event_name\":\"PreToolUse\",\"tool_name\":\"Bash\",\"tool_input\":{\"command\":\"echo probe\"},\"session_id\":\"hook-cost\",\"tool_use_id\":\"%s\"}\n" "$i" | "$1" >/dev/null
  done
' _ "$hook_path" "$count"
/usr/bin/time -f 'pipe_control_seconds=%e' bash -c '
  for ((i=1; i<=$1; i++)); do
    printf "{\"hook_event_name\":\"PreToolUse\",\"tool_name\":\"Bash\",\"tool_input\":{\"command\":\"echo probe\"},\"session_id\":\"hook-cost\",\"tool_use_id\":\"%s\"}\n" "$i" | true
  done
' _ "$count"
