"""Score each arm against the reference labels. Reference is a model, not a human:
sampled disagreements were read by hand and the reference was right in every case
checked, but that is a spot check and not a guarantee."""
import json, os, sys

STALLS = os.environ.get('STALLS_DIR', '/tmp/stalls')
rows  = [json.loads(l) for l in open(f'{STALLS}/labels.jsonl')]
pred  = [l.split('\t') for l in open(f'{STALLS}/predictions.tsv').read().strip().split('\n') if l]
arms  = {'phrase list (shipping)': lambda i: pred[i][2] == '1'}

for name, path in [(a.split('=')[0], a.split('=')[1]) for a in sys.argv[1:]]:
    lab = [json.loads(l) for l in open(path)]
    arms[name] = (lambda L: lambda i: bool(L[i]['oracle'].get('names_next_step'))
                                      and not bool(L[i]['oracle'].get('asks_question')))(lab)
    arms[name + ' :question'] = (lambda L: lambda i: bool(L[i]['oracle'].get('asks_question')))(lab)

def truth(i):
    o = rows[i]['oracle']
    return bool(o.get('names_next_step')) and not bool(o.get('asks_question'))

print(f"Decision: continue this stalled session?   n={len(rows)}\n")
for name, fn in arms.items():
    if name.endswith(':question'): continue
    tp = sum(1 for i in range(len(rows)) if fn(i) and truth(i))
    fp = sum(1 for i in range(len(rows)) if fn(i) and not truth(i))
    fnn = sum(1 for i in range(len(rows)) if not fn(i) and truth(i))
    p = tp/(tp+fp) if tp+fp else 0.0
    r = tp/(tp+fnn) if tp+fnn else 0.0
    f1 = 2*p*r/(p+r) if p+r else 0.0
    print(f"{name:28} precision={p:.2f} recall={r:.2f} F1={f1:.2f}  (fires {tp+fp}, wrong {fp}, misses {fnn})")

print("\nSafety guard -- a question must stop it:")
asked = [i for i in range(len(rows)) if rows[i]['oracle'].get('asks_question')]
print(f"{'phrase list':28} {sum(1 for i in asked if pred[i][1]=='1')}/{len(asked)}")
for name, fn in arms.items():
    if name.endswith(':question'):
        print(f"{name[:-9]:28} {sum(1 for i in asked if fn(i))}/{len(asked)}")
