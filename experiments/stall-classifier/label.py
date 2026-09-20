import json, os, urllib.request, sys, time

KEY = open(os.path.expanduser('~/Documents/API_KEYS/nvidia-inference.txt')).read().strip()
URL = 'https://inference-api.nvidia.com/v1/messages'
MODEL = os.environ.get("ARM_MODEL", "azure/anthropic/claude-opus-5")
OUT = os.environ.get("ARM_OUT", "/tmp/stalls/labels.jsonl")

rows = [json.loads(l) for l in open('/tmp/stalls/corpus.jsonl')]

# The oracle sees the same text the shipping predicate sees, and is told nothing
# about how that predicate works: no phrase list, no hint of what it looks for.
# Otherwise this measures agreement with my own guesses rather than the truth.
QUESTION = """You are labelling the final message of a coding-agent turn. The agent stopped
after this message and waited for a human.

For each message answer two independent questions:

1. names_next_step: does the message state a specific next action ON THE TASK that the
   agent itself could carry out without any decision from a human? A vague sign-off
   ("let me know if you need anything"), a pure completion report with nothing left,
   or a suggestion that the HUMAN do something are all false.
2. asks_question: does the message ask the human anything, or request a decision,
   approval, preference, or clarification? Include implicit asks ("let me know which
   you prefer", "I can do A or B").

Return ONLY a JSON array, one object per message, in order:
[{"i":0,"names_next_step":true,"asks_question":false}, ...]"""

def ask(batch, start):
    body = {"model": MODEL, "max_tokens": 4000,
            "messages": [{"role":"user","content": QUESTION + "\n\n" + "\n\n".join(
                f"--- message {start+j} ---\n{m}" for j,m in enumerate(batch))}]}
    req = urllib.request.Request(URL, data=json.dumps(body).encode(),
        headers={'Authorization': f'Bearer {KEY}', 'content-type':'application/json',
                 'anthropic-version':'2023-06-01'})
    with urllib.request.urlopen(req, timeout=180) as r:
        d = json.loads(r.read())
    txt = ''.join(c.get('text','') for c in d.get('content',[]) if c.get('type')=='text')
    s, e = txt.find('['), txt.rfind(']')
    return json.loads(txt[s:e+1]), d.get('usage',{})

CAP = 4000
out, usage_in = [], 0
B = 10
for i in range(0, len(rows), B):
    batch = [r['message'][-CAP:] for r in rows[i:i+B]]
    for attempt in range(3):
        try:
            labels, u = ask(batch, i)
            usage_in += u.get('input_tokens',0)
            out += labels[:len(batch)]
            break
        except Exception as ex:
            if attempt == 2: print(f"batch {i} failed: {ex}", file=sys.stderr); out += [{}]*len(batch)
            else: time.sleep(3)
    print(f"\r{len(out)}/{len(rows)}", end='', file=sys.stderr)

print(file=sys.stderr)
with open(OUT, "w") as f:
    for r, l in zip(rows, out):
        f.write(json.dumps({**r, 'oracle': l})+'\n')
print(f"labelled {sum(1 for l in out if l)}/{len(rows)} with {MODEL}; input tokens {usage_in}")
