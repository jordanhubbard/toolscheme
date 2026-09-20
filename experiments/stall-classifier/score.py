"""Score each arm against the reference labels.

The reference is a model, not a person: sampled disagreements were read by hand
and the reference was right in every case checked, which is a spot check and not
a guarantee.

Arms are passed as name=path. A .tsv is the output of a predict*.scm run
(names/asks/decision, optionally a source column); a .jsonl is a label.py run.

    python3 score.py "chat=/tmp/stalls/predictions-shipping.tsv" \
                     "von=/tmp/stalls/predictions-von.tsv"
"""
import json, os, sys

STALLS = os.environ.get('STALLS_DIR', '/tmp/stalls')
rows = [json.loads(l) for l in open(f'{STALLS}/labels.jsonl')]


def truth(i):
    o = rows[i]['oracle']
    return bool(o.get('names_next_step')) and not bool(o.get('asks_question'))


def load(path):
    """-> (decides, says_question, sources)"""
    if path.endswith('.tsv'):
        cols = [l.split('\t') for l in open(path).read().strip().split('\n') if l]
        return ([c[2] == '1' for c in cols],
                [c[1] == '1' for c in cols],
                [c[3] if len(c) > 3 else '' for c in cols])
    lab = [json.loads(l) for l in open(path)]
    key = 'oracle' if 'oracle' in lab[0] else 'cheap'
    return ([bool(x[key].get('names_next_step')) and not bool(x[key].get('asks_question'))
             for x in lab],
            [bool(x[key].get('asks_question')) for x in lab],
            [''] * len(lab))


arms = [('phrase list', f'{STALLS}/predictions.tsv')]
arms += [(a.split('=', 1)[0], a.split('=', 1)[1]) for a in sys.argv[1:]]

asked = [i for i in range(len(rows)) if rows[i]['oracle'].get('asks_question')]
print(f"n={len(rows)}   reference = Opus 5   ({len(asked)} of them ask something)\n")
print(f"{'arm':<22}{'prec':>6}{'recall':>8}{'F1':>7}   fires  wrong  misses   questions")

for name, path in arms:
    decides, says_q, _ = load(path)
    if len(decides) != len(rows):
        print(f"{name:<22}  SKIPPED: {len(decides)} predictions for {len(rows)} stalls")
        continue
    tp = sum(1 for i in range(len(rows)) if decides[i] and truth(i))
    fp = sum(1 for i in range(len(rows)) if decides[i] and not truth(i))
    fn = sum(1 for i in range(len(rows)) if not decides[i] and truth(i))
    p = tp / (tp + fp) if tp + fp else 0.0
    r = tp / (tp + fn) if tp + fn else 0.0
    f1 = 2 * p * r / (p + r) if p + r else 0.0
    caught = sum(1 for i in asked if says_q[i])
    print(f"{name:<22}{p:>6.2f}{r:>8.2f}{f1:>7.2f}{tp+fp:>8}{fp:>7}{fn:>8}"
          f"{caught:>9}/{len(asked)}")

# Idle time is the reason any of this exists, so it is reported alongside.
print()
for name, path in arms:
    decides, _, _ = load(path)
    if len(decides) != len(rows):
        continue
    hours = sum(rows[i]['gap_seconds'] for i in range(len(rows))
                if decides[i] and rows[i]['gap_seconds'] < 21600) / 3600
    print(f"  {name:<22} would recover {hours:5.1f} h of idle (gaps under 6h)")
