#!/bin/sh
# Does steering an agent toward structured tools save tokens?
#
# The original premise: agents call Unix tools constantly, those tools emit text
# written for humans, and the model pays to parse it. A bounded structured result
# should cost less. This measures that with the agent's own accounting rather than
# a proxy -- Codex records input, output and cached tokens per turn in its rollout.
#
# Both arms get the same investigation over the same corpus. Only the instruction
# differs, and each arm has its own CODEX_HOME so the control genuinely lacks it.
set -e
TRIALS="${1:-2}"
HOOK="$HOME/.local/share/toolscheme/hooks/observe.sh"
WORK=${WORK:-/tmp/token-experiment}
CORPUS=${CORPUS:-/home/jkh/Src/nanolang}

TASK='Investigate this repository and answer, in under 150 words: what does its
documentation say about (a) the type checker, (b) vectorization, and (c) memory
safety? Cite the file each answer came from. Work only from files in docs/.'

NOTE='
## Searching and reading

`toolscheme` is on PATH. It returns bounded, structured results instead of text
meant for a human, in one call rather than a search followed by several reads:

    toolscheme -e '"'"'(tool-invoke "search-read" (quote ((pattern "PATTERN") (source (glob "docs/**/*.md")) (limit 12) (context 6))))'"'"'

It returns every match with the surrounding lines of each file, already bounded.
Prefer it to `grep` followed by reading whole files.
'

rm -rf "$WORK"; mkdir -p "$WORK"
for arm in plain steered; do
  for i in $(seq 1 "$TRIALS"); do
    home="$WORK/home-$arm-$i"; state="$WORK/state-$arm-$i"
    mkdir -p "$home" "$state"
    ln -sf "$HOME/.codex/auth.json" "$home/auth.json"
    printf 'model = "gpt-6-astra"\napproval_policy = "never"\n\n' > "$home/config.toml"
    printf '[[hooks.PreToolUse]]\nmatcher = "*"\n[[hooks.PreToolUse.hooks]]\ntype = "command"\ncommand = "%s"\n' "$HOOK" >> "$home/config.toml"
    printf '[[hooks.PostToolUse]]\nmatcher = "*"\n[[hooks.PostToolUse.hooks]]\ntype = "command"\ncommand = "%s"\n' "$HOOK" >> "$home/config.toml"
    [ "$arm" = steered ] && printf '%s' "$NOTE" > "$home/AGENTS.md"
    CODEX_HOME="$home" TOOLSCHEME_STATE="$state" timeout 600 codex exec \
        --sandbox danger-full-access --skip-git-repo-check \
        --dangerously-bypass-hook-trust --cd "$CORPUS" \
        "$TASK" < /dev/null > "$WORK/log-$arm-$i.txt" 2>&1 || true
    echo "  $arm trial $i"
  done
done
echo "runs complete: $WORK"
