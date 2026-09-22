"""Ask Laya how much a command is about to print.

The state here is one short command line, which is the shape these models are
built for -- unlike the stall corpus, where the state was a long sign-off full
of code and citation XML and every local model scored around half the remote
one. If a System One model is going to earn a place on the per-call path, it
should show it here.

Both primitives are asked in the same forward pass, since questions are free and
round trips are not.
"""
import json, os, time

MODEL = os.environ.get('LAYA_MODEL', 'convaiinnovations/laya')
SUBFOLDER = os.environ.get('LAYA_SUBFOLDER', '') or None
OUT = os.environ.get('LAYA_OUT', '/tmp/sizes/laya.jsonl')

QUESTIONS = {
    "large_output": {
        "type": "noul",
        "instructions": "Will running this shell command print a large amount of text, more than about fifty lines?",
    },
    "volume": {
        "type": "score",
        "instructions": "How much text will running this shell command print?",
        "criteria": ["almost nothing", "a few lines", "a screenful", "far more than a screenful"],
    },
}

import laya  # noqa: E402

t0 = time.time()
agent = laya.load(MODEL, subfolder=SUBFOLDER)
print(f"loaded in {time.time()-t0:.1f}s")

rows = [json.loads(l) for l in open('/tmp/sizes/corpus.jsonl')]
out, times = [], []
for i, r in enumerate(rows):
    t = time.time()
    try:
        result = agent.predict(r['command'], QUESTIONS)
        times.append(time.time() - t)
        a = result['answers']
        out.append({'noul': a['large_output'].get('noul'),
                    'score': a['volume'].get('score')})
    except Exception as ex:
        out.append({'error': str(ex)})
    if i % 200 == 0:
        print(f"\r{i}/{len(rows)}", end='', flush=True)

print()
with open(OUT, 'w') as f:
    for r, o in zip(rows, out):
        f.write(json.dumps({**r, 'laya': o}) + '\n')
times.sort()
if times:
    print(f"{len(times)} calls  p50={times[len(times)//2]*1000:.0f}ms  "
          f"p95={times[int(len(times)*0.95)]*1000:.0f}ms  total={sum(times):.0f}s")
