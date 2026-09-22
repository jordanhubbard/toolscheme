"""Capture raw noul probabilities from Laya, in the same shape as raw-von.py.

Laya is a library rather than a server, so this calls it in process. The four
questions are word-for-word the ones the shipping classifier asks, so the only
thing varying between this and the Von run is the model.
"""
import json, os, time

MODEL = os.environ.get('LAYA_MODEL', 'convaiinnovations/laya')
# The repo bundles several checkpoints; "typed-decisions" is the one trained for
# exactly this shape of question. Empty means the English checkpoint at the root.
SUBFOLDER = os.environ.get('LAYA_SUBFOLDER', '') or None
OUT = os.environ.get('LAYA_OUT', '/tmp/stalls/raw-laya.jsonl')
# Laya's signature documents noul as instructions-only, unlike Von. Criteria are
# sent only when asked for, so both wordings can be measured rather than assumed.
WITH_CRITERIA = os.environ.get('LAYA_CRITERIA', '0') == '1'


def noul(instructions, when_true, when_false):
    q = {"type": "noul", "instructions": instructions}
    if WITH_CRITERIA:
        q["criteria"] = {"true": when_true, "false": when_false}
    return q


QUESTIONS = {
    "names_next_step": noul(
        "Does the message state a specific next action on the task that the agent itself could carry out with no decision from a human?",
        "A concrete next action on the task, needing nobody",
        "A sign-off, a finished report, or work for the human to do"),
    "asks_question": noul(
        "Does the message ask the human anything, or request a decision, approval, preference, or clarification?",
        "Anything is asked of the human",
        "Nothing is asked of the human"),
    "awaits_human": noul(
        "Does the message say it is waiting on a person, a review, an approval, or an external party before it can proceed?",
        "Blocked on someone or something outside the agent",
        "Not waiting on anyone"),
    "needs_choice": noul(
        "Would carrying on require choosing between alternatives the message leaves open?",
        "An open choice is left unmade",
        "No choice is left open"),
}

import laya  # noqa: E402  -- after the config, so a bad model id fails fast

t0 = time.time()
agent = laya.load(MODEL, subfolder=SUBFOLDER)
print(f"loaded {MODEL}{'/' + SUBFOLDER if SUBFOLDER else ''} in {time.time()-t0:.1f}s")

rows = [json.loads(l) for l in open('/tmp/stalls/corpus.jsonl')]
out, times = [], []
for i, r in enumerate(rows):
    t = time.time()
    try:
        result = agent.predict(r['message'], QUESTIONS)
        times.append(time.time() - t)
        answers = result["answers"] if isinstance(result, dict) else result.answers
        out.append({k: (v.get('noul') if isinstance(v, dict) else getattr(v, 'noul', None))
                    for k, v in answers.items()})
    except Exception as ex:
        out.append({'error': str(ex)})
    print(f"\r{i+1}/{len(rows)}", end='')

print()
with open(OUT, 'w') as f:
    for r, o in zip(rows, out):
        f.write(json.dumps({**r, 'von': o}) + '\n')   # 'von' key so the sweep is shared
times.sort()
if times:
    print(f"{len(times)} calls  p50={times[len(times)//2]*1000:.0f}ms  "
          f"p95={times[int(len(times)*0.95)]*1000:.0f}ms  total={sum(times):.1f}s")
