"""Cheap predictors of "will this command print a lot?", for a model to beat.

A classifier on the per-call path only earns its latency if it beats what the
same information gives you for free. Two of these cost nothing at all: the
program name, and the fact that a pipeline ending in `head` cannot print much
whatever precedes it.

Leave-one-out throughout, so a command never contributes to its own prediction.
"""
import json, os, re, collections, sys

THRESHOLD = int(os.environ.get('THRESHOLD', '2000'))
rows = [json.loads(l) for l in open('/tmp/sizes/corpus.jsonl')]
for r in rows:
    r['big'] = r['bytes'] >= THRESHOLD

WORD = re.compile(r'[A-Za-z0-9_./-]+')


def programs(command):
    """Every program invoked, in order, across pipes and operators."""
    out = []
    for segment in re.split(r'\|\||&&|\||;|\n', command):
        for token in WORD.findall(segment.strip()):
            if '=' in token:
                continue
            out.append(os.path.basename(token))
            break
    return out or ['?']


for r in rows:
    progs = programs(r['command'])
    r['first'] = progs[0]
    r['last'] = progs[-1]


def score(name, predict):
    tp = fp = fn = tn = 0
    for i, r in enumerate(rows):
        p = predict(i, r)
        if p and r['big']: tp += 1
        elif p and not r['big']: fp += 1
        elif not p and r['big']: fn += 1
        else: tn += 1
    prec = tp / (tp + fp) if tp + fp else 0.0
    rec = tp / (tp + fn) if tp + fn else 0.0
    f1 = 2 * prec * rec / (prec + rec) if prec + rec else 0.0
    acc = (tp + tn) / len(rows)
    print(f"{name:<34}{prec:>7.2f}{rec:>8.2f}{f1:>7.2f}{acc:>9.2f}")
    return f1


# Leave-one-out tables: total bytes and count per key, minus the row itself.
def table(key):
    total, count = collections.Counter(), collections.Counter()
    for r in rows:
        total[r[key]] += r['bytes']
        count[r[key]] += 1
    return total, count


def memo(key):
    total, count = table(key)
    def predict(i, r):
        k = r[key]
        n = count[k] - 1
        if n <= 0:
            return False            # unseen key: no opinion, so predict small
        return (total[k] - r['bytes']) / n >= THRESHOLD
    return predict


BOUNDED = {'head', 'tail', 'wc', 'true', 'test', 'touch', 'mkdir', 'cd', 'echo'}

print(f"corpus: {len(rows)} commands, threshold {THRESHOLD} bytes, "
      f"{sum(1 for r in rows if r['big'])} positive "
      f"({100*sum(1 for r in rows if r['big'])/len(rows):.0f}%)\n")
print(f"{'predictor':<34}{'prec':>7}{'recall':>8}{'F1':>7}{'accuracy':>9}")
score("always small", lambda i, r: False)
score("always large", lambda i, r: True)
score("first program (mean bytes)", memo('first'))
score("last program (mean bytes)", memo('last'))
score("pipeline ends in a bounding tool",
      lambda i, r: r['last'] not in BOUNDED)
last = memo('last')
score("both: bounded tool, else memo",
      lambda i, r: False if r['last'] in BOUNDED else last(i, r))
