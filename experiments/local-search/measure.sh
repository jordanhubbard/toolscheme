#!/usr/bin/env bash
# Compare launched commands on this checkout. Run from the repository root.
# COUNT=100 experiments/local-search/measure.sh
# The commands have similar scope but differ in output format and ignore rules.
set -euo pipefail

count=${COUNT:-100}
root=$(cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$root"

measure() {
  local label=$1 start end
  shift
  start=$(date +%s%N)
  for ((i=0; i<count; i++)); do "$@" >/dev/null; done
  end=$(date +%s%N)
  printf '%s calls=%d total_ms=%d mean_ms=%d\n' "$label" "$count" \
    "$(((end-start)/1000000))" "$(((end-start)/1000000/count))"
}

printf 'rg_matches=%s\n' "$(rg -n -F --no-ignore --glob '*.cpp' cache_lookup . | wc -l)"
printf 'grep_matches=%s\n' "$(grep -rFn --include='*.cpp' --exclude-dir=.git cache_lookup . | wc -l)"
printf 'toolscheme_matches=%s\n' "$(./toolscheme -e '(field-ref (search "cache_lookup" (list ".") (quote ((literal #t) (include ("*.cpp")) (limit 1000)))) (quote count) 0)')"
printf 'find_files=%s\n' "$(find . -type f -name '*.cpp' -not -path './.git/*' | wc -l)"
printf 'toolscheme_glob_files=%s\n' "$(./toolscheme -e '(field-ref (glob "**/*.cpp") (quote count) 0)')"

measure rg rg -n -F --no-ignore --glob '*.cpp' cache_lookup .
measure grep grep -rFn --include='*.cpp' --exclude-dir=.git cache_lookup .
measure toolscheme_search ./toolscheme -e '(search "cache_lookup" (list ".") (quote ((literal #t) (include ("*.cpp")) (limit 1000))))'
measure find find . -type f -name '*.cpp' -not -path './.git/*'
measure toolscheme_glob ./toolscheme -e '(glob "**/*.cpp")'
