"""Capture raw noul probabilities from a System One endpoint, one row per stall.

Thresholded verdicts hide where a wrong answer came from: a guard that trips at
0.31 and one that trips at 0.99 look identical afterwards. Keeping the numbers
lets the thresholds be chosen from the corpus instead of from taste.
"""
import json, os, time, urllib.request

URL = os.environ.get('VON_URL', 'http://127.0.0.1:8000/v1/systemone')
MODEL = os.environ.get('VON_MODEL', 'von-1.0.0')
OUT = os.environ.get('VON_OUT', '/tmp/stalls/raw-von2.jsonl')

def noul(instructions, when_true, when_false):
    return {"type": "noul", "instructions": instructions,
            "criteria": {"true": when_true, "false": when_false}}

QUESTIONS = {
    "names_next_step": noul(
        "Does this message describe work that is still unfinished and about to be done next?",
        "Names specific work that has NOT been done yet",
        "Reports only completed work, or signs off, or hands over to the human"),
    "asks_question": noul(
        "Does the message ask the human anything, or request a decision, approval, preference, or clarification?",
        "Anything is asked of the human",
        "Nothing is asked of the human"),
    "awaits_human": noul(
        "Does the message say it is waiting on a person, a review, an approval, or an external party before it can proceed?",
        "Blocked on someone or something outside the agent",
        "Not waiting on anyone"),
    "needs_choice": noul(
        "Does the message offer the human two or more options to pick between?",
        "Presents alternatives for the human to choose from",
        "Presents no alternatives"),
}

rows = [json.loads(l) for l in open('/tmp/stalls/corpus.jsonl')]
out, times = [], []
for i, r in enumerate(rows):
    body = json.dumps({"model": MODEL, "state": r['message'], "questions": QUESTIONS}).encode()
    req = urllib.request.Request(URL, data=body, headers={'content-type': 'application/json'})
    t = time.time()
    try:
        with urllib.request.urlopen(req, timeout=60) as resp:
            d = json.loads(resp.read())
        times.append(time.time() - t)
        out.append({k: v.get('noul') for k, v in d.get('answers', {}).items()})
    except Exception as ex:
        out.append({'error': str(ex)})
    print(f"\r{i+1}/{len(rows)}", end='')

print()
with open(OUT, 'w') as f:
    for r, o in zip(rows, out):
        f.write(json.dumps({**r, 'von': o}) + '\n')
times.sort()
print(f"{len(times)} calls  p50={times[len(times)//2]*1000:.0f}ms  "
      f"p95={times[int(len(times)*0.95)]*1000:.0f}ms  total={sum(times):.1f}s")
