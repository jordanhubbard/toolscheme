"""Is there signal in a command beyond which program it runs?

The memo table keys on the program name and scores F1 0.57, but its precision is
0.45: it cannot tell `grep -rn x /` from `grep -n x small.c`. If a capable model
reading the arguments cannot beat it either, the table is near the ceiling for
this task and there is nothing for a faster or bigger local model to recover.

This is a ceiling probe, not a candidate. A remote model at ~1.7s per call can
never sit on the per-call path; it is here to say whether the headroom exists.
"""
import json, os, random, urllib.request, sys

# Which credential supplies the key also decides how it is presented: Anthropic
# directly takes x-api-key, a gateway takes a bearer token, and sending the wrong
# one is a 401 that reads like a bad key rather than a bad header.
KEY_FILE = os.environ.get('CEILING_KEY_FILE',
                          '~/Documents/API_KEYS/nvidia-inference.txt')
KEY = open(os.path.expanduser(KEY_FILE)).read().strip()
URL = os.environ.get('CEILING_URL', 'https://inference-api.nvidia.com/v1/messages')
MODEL = os.environ.get('CEILING_MODEL', 'azure/anthropic/claude-haiku-4-5')
AUTH = ({'x-api-key': KEY} if 'api.anthropic.com' in URL
        else {'Authorization': f'Bearer {KEY}'})
SAMPLE = int(os.environ.get('CEILING_SAMPLE', '400'))
THRESHOLD = int(os.environ.get('THRESHOLD', '2000'))
BATCH = 20

rows = [json.loads(l) for l in open('/tmp/sizes/corpus.jsonl')]
random.seed(11)                      # fixed, so the sample is the same every run
sample = random.sample(rows, min(SAMPLE, len(rows)))

PROMPT = f"""For each shell command, judge whether running it would print {THRESHOLD}
bytes or more of output. Think about what the command actually does: how much the
program typically emits, how broad the paths and globs are, and whether a pipeline
stage bounds the result.

Return ONLY a JSON array of objects, one per command, echoing its number:
[{{"i": 0, "big": true}}, {{"i": 1, "big": false}}, ...]

Every number listed must appear exactly once."""


def ask(batch):
    # A command can contain newlines, and a numbered listing that spans lines
    # invites the model to answer for the wrong number of items -- which it did,
    # returning 19 answers for 20 commands. One line each, flattened.
    listing = "\n".join(f"{i}. {c.replace(chr(10), ' ; ')}" for i, c in enumerate(batch))
    body = {"model": MODEL, "max_tokens": 2000,
            "messages": [{"role": "user", "content":
                PROMPT + f"\n\nReturn exactly {len(batch)} booleans.\n\n" + listing}]}
    req = urllib.request.Request(URL, data=json.dumps(body).encode(),
        headers={**AUTH, 'content-type': 'application/json',
                 'anthropic-version': '2023-06-01'})
    with urllib.request.urlopen(req, timeout=180) as r:
        d = json.loads(r.read())
    txt = ''.join(c.get('text', '') for c in d.get('content', []) if c.get('type') == 'text')
    s, e = txt.find('['), txt.rfind(']')
    out = json.loads(txt[s:e + 1])
    # Keyed by the index the model echoes back, so a dropped or duplicated entry
    # is detected here rather than silently shifting every later label onto the
    # wrong command. Positional answers cost two runs of this before that showed.
    by_index = {int(o['i']): bool(o['big']) for o in out if isinstance(o, dict)}
    missing = [i for i in range(len(batch)) if i not in by_index]
    if missing:
        raise ValueError(f"missing answers for {missing}")
    return [by_index[i] for i in range(len(batch))]


import time

# A failed batch padded with False reads as "the model thinks nothing is large",
# which is indistinguishable from a real answer and scored 0.00 precision on the
# first run of this before the 429s were noticed. Failures are now retried and
# then fatal.
preds, failures = [], 0
for i in range(0, len(sample), BATCH):
    batch = [r['command'] for r in sample[i:i + BATCH]]
    got = None
    for attempt in range(6):
        try:
            # Assigned only once the count is right. Assigning first and checking
            # after left a short batch in place when every retry failed the same
            # way, which shifts every later prediction onto the wrong command --
            # invisible in the output and fatal to the result.
            candidate = ask(batch)
            if len(candidate) != len(batch):
                raise ValueError(f"{len(candidate)} answers for {len(batch)} commands")
            got = candidate
            break
        except Exception as ex:
            failures += 1
            wait = 2 ** attempt
            print(f"\nbatch {i} attempt {attempt}: {ex}; retrying in {wait}s",
                  file=sys.stderr)
            time.sleep(wait)
    if got is None:
        sys.exit(f"batch {i} never succeeded; refusing to report padded results")
    preds += got
    print(f"\r{len(preds)}/{len(sample)}", end='', file=sys.stderr)
    time.sleep(1.0)                  # the gateway rate-limits well below this load

print(f"\n{failures} transient failures retried", file=sys.stderr)
with open('/tmp/sizes/ceiling.jsonl', 'w') as f:
    for r, p in zip(sample, preds):
        f.write(json.dumps({**r, 'model_big': p}) + '\n')
print(f"wrote {len(preds)} predictions from {MODEL}")
