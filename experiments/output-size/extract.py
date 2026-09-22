"""Pair every shell command an agent ran with the number of bytes it got back.

The stall corpus needed a model to label it, because "does this message name a
next step" has no ground truth on disk. This one does not: the output size is
recorded, so the label is a fact rather than another model's opinion. That makes
it the better corpus of the two for deciding whether a classifier is worth its
place on the per-call path.
"""
import json, glob, os, collections

def bytes_of(result):
    if isinstance(result, str):
        return len(result)
    if isinstance(result, dict):
        n = 0
        for key in ('stdout', 'stderr', 'output', 'content', 'text'):
            v = result.get(key)
            if isinstance(v, str):
                n += len(v)
            elif isinstance(v, list):
                for item in v:
                    if isinstance(item, dict) and isinstance(item.get('text'), str):
                        n += len(item['text'])
        return n
    return None


def claude(path, rows):
    calls = {}
    for line in open(path, errors='replace'):
        try:
            d = json.loads(line)
        except Exception:
            continue
        m = d.get('message')
        if isinstance(m, dict) and isinstance(m.get('content'), list):
            for c in m['content']:
                if not isinstance(c, dict):
                    continue
                if c.get('type') == 'tool_use' and c.get('name') == 'Bash':
                    cmd = (c.get('input') or {}).get('command')
                    if isinstance(cmd, str) and cmd.strip():
                        calls[c.get('id')] = cmd
                elif c.get('type') == 'tool_result':
                    cid = c.get('tool_use_id')
                    if cid in calls:
                        n = bytes_of(d.get('toolUseResult'))
                        if n is None:
                            n = bytes_of(c.get('content'))
                        if n is not None:
                            rows.append({'agent': 'claude', 'command': calls.pop(cid),
                                         'bytes': n})


def codex(path, rows):
    calls = {}
    for line in open(path, errors='replace'):
        try:
            d = json.loads(line)
        except Exception:
            continue
        p = d.get('payload') or {}
        t = p.get('type')
        if t in ('function_call', 'custom_tool_call'):
            raw = p.get('arguments') or p.get('input') or ''
            cmd = None
            if isinstance(raw, str):
                try:
                    parsed = json.loads(raw)
                    cmd = parsed.get('command') or parsed.get('cmd')
                    if isinstance(cmd, list):
                        cmd = ' '.join(cmd)
                except Exception:
                    cmd = None
            if isinstance(cmd, str) and cmd.strip():
                calls[p.get('call_id')] = cmd
        elif t in ('function_call_output', 'custom_tool_call_output'):
            cid = p.get('call_id')
            if cid in calls:
                out = p.get('output')
                n = bytes_of(out)
                if n is None and isinstance(out, str):
                    n = len(out)
                if n is not None:
                    rows.append({'agent': 'codex', 'command': calls.pop(cid), 'bytes': n})


rows = []
for path in glob.glob(os.path.expanduser('~/.claude/projects/*/*.jsonl')):
    claude(path, rows)
for path in glob.glob(os.path.expanduser('~/.codex/sessions/**/rollout-*.jsonl'),
                      recursive=True):
    codex(path, rows)

# A command repeated verbatim is one fact observed twice; keeping both would let
# whatever is most-run dominate the score.
seen, uniq = set(), []
for r in rows:
    if r['command'] in seen:
        continue
    seen.add(r['command'])
    uniq.append(r)

with open('/tmp/sizes/corpus.jsonl', 'w') as f:
    for r in uniq:
        f.write(json.dumps(r) + '\n')

sizes = sorted(r['bytes'] for r in uniq)
print(f"{len(rows)} calls, {len(uniq)} distinct commands")
print("by agent:", collections.Counter(r['agent'] for r in uniq))
if sizes:
    def pct(p):
        return sizes[min(len(sizes) - 1, int(len(sizes) * p))]
    print(f"output bytes  p10={pct(.1)}  p50={pct(.5)}  p90={pct(.9)}  max={sizes[-1]}")
    for t in (1000, 2000, 4000, 8000):
        print(f"  over {t:>5} bytes: {sum(1 for s in sizes if s >= t):4}/{len(sizes)}"
              f"  ({100*sum(1 for s in sizes if s >= t)/len(sizes):.0f}%)")
