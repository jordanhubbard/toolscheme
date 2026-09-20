import json, os, glob, datetime, sys

def ts(s):
    if not s: return None
    try: return datetime.datetime.fromisoformat(s.replace('Z','+00:00')).timestamp()
    except Exception: return None

def codex_msg(d):
    p = d.get('payload') or {}
    t = p.get('type')
    if t == 'agent_message': return 'assistant', p.get('message','')
    if t == 'user_message':  return 'user', p.get('message','')
    if t == 'message':
        c = p.get('content') or [{}]
        txt = ''.join(x.get('text','') for x in c if isinstance(x,dict))
        return p.get('role',''), txt
    return None, None

def claude_msg(d):
    m = d.get('message')
    if not isinstance(m, dict): return None, None
    role = m.get('role','')
    c = m.get('content')
    if isinstance(c, str): return role, c
    if isinstance(c, list):
        txt = ''.join(x.get('text','') for x in c if isinstance(x,dict) and x.get('type')=='text')
        return role, txt
    return None, None

def walk(path, parse, agent):
    seq = []
    try:
        with open(path, errors='replace') as f:
            for line in f:
                line = line.strip()
                if not line: continue
                try: d = json.loads(line)
                except Exception: continue
                role, txt = parse(d)
                if role not in ('assistant','user'): continue
                t = ts(d.get('timestamp'))
                if t is None: continue
                seq.append((t, role, txt or ''))
    except Exception: return []
    out = []
    # Only the assistant message *immediately* before a user reply ends a turn.
    # Counting every assistant message that happens to precede one counts mid-turn
    # commentary as a stall, which inflated the first run of this by ~50x.
    for i,(t,role,txt) in enumerate(seq):
        if role != 'user': continue
        prev = None
        for j in range(i-1, -1, -1):
            if seq[j][1] == 'assistant':
                prev = seq[j]; break
            break                          # anything else between: not a turn end
        if not prev or not prev[2].strip(): continue
        gap = t - prev[0]
        if gap >= 90:
            out.append({'agent':agent,'session':os.path.basename(path),
                        'gap_seconds':round(gap,1),'message':prev[2].strip()})
    return out

rows = []
for p in glob.glob(os.path.expanduser('~/.codex/sessions/**/rollout-*.jsonl'), recursive=True):
    rows += walk(p, codex_msg, 'codex')
for p in glob.glob(os.path.expanduser('~/.claude/projects/*/*.jsonl')):
    rows += walk(p, claude_msg, 'claude')

# one stall per distinct final message: near-duplicates across resumed threads
# would otherwise weight a single phrasing many times over
seen, uniq = set(), []
for r in rows:
    k = r['message'][:400]
    if k in seen: continue
    seen.add(k); uniq.append(r)

with open('/tmp/stalls/corpus.jsonl','w') as f:
    for r in uniq: f.write(json.dumps(r)+'\n')
print(f"stalls: {len(rows)} raw, {len(uniq)} distinct")
from collections import Counter
print("by agent:", Counter(r['agent'] for r in uniq))
print("median gap (min):", round(sorted(r['gap_seconds'] for r in uniq)[len(uniq)//2]/60,1))
