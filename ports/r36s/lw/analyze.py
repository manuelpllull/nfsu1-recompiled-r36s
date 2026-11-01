#!/usr/bin/env python3
"""x87 stack-depth analysis of the lifted C (v3) for the local x87 window (arm64_guest.h NFS_X87_LW).
Per function: depth relative to entry at every statement (one x86 instruction per line), consistency at
labels and returns, and the depth change it leaves (its delta). Calls:
 - RECOMP_CALL(sub_X): the callee's delta (fixpoint over the call graph);
 - RECOMP_ICALL(...): inferred per site from the instructions after it: +1 when one of them needs more values
   than were pushed since the call, before any label, call, jump or return; else 0. Functions with indirect
   calls must never go below their entry depth, so a wrong guess makes the function ineligible;
 - RECOMP_ITAIL(constant): a tail jump: ends the path with the target's delta;
 - `default: RECOMP_ITAIL(_jt)` (jump-table miss) and other ITAILs end the path; they are left out of the
   delta (a miss never happens in practice; a dynamic tail target is unknown -> the delta is unknown).
   At run time an LW function returns straight after any ITAIL (the callee's state is the final state).
Output: info.json {name: {fp, lw, delta, icd: [per-ICALL delta in text order], why}}."""
import re, glob, sys, json, collections
GEN = sys.argv[1]
FUNC = re.compile(r'^void (sub_[0-9A-F]{8})\(void\) \{', re.M)
LABEL = re.compile(r'^(L_[0-9A-F]{8}):')
CALL = re.compile(r'RECOMP_CALL\((sub_[0-9A-F]{8})\)')
ICALL = re.compile(r'RECOMP_ICALL(?:_LW\(-?\d+, *|\()')
ITAILC = re.compile(r'RECOMP_ITAIL\(0x([0-9A-Fa-f]{8})u?\)')
GOTO = re.compile(r'goto (L_[0-9A-F]{8});')
RD = re.compile(r'(?:_st|g_st_i64)\[(\d)\]|fp_xch\((\d)\)')
BAD = ('RECOMP_HOOK(', 'CPUID', 'RECOMP_UNIMPL', '__asm', 'RECOMP_REGS_OUT()', 'RECOMP_REGS_IN()')
funcs = {}
for f in sorted(glob.glob(GEN + '/recomp_*.c')):
    s = open(f).read()
    ms = list(FUNC.finditer(s))
    for i, m in enumerate(ms):
        funcs[m.group(1)] = s[m.end():ms[i+1].start() if i+1 < len(ms) else len(s)]
def code(t):
    i = t.find('/*')
    return t if i < 0 else t[:i]
parsed = {}
def parse(name):
    if name not in parsed:
        st = []
        for line in funcs[name].split('\n'):
            t = line.strip()
            m = LABEL.match(t)
            if m: st.append(('label', m.group(1))); t = t[m.end():].strip()
            c = code(t).strip()
            if c: st.append(('stmt', c))
        parsed[name] = st
    return parsed[name]
def ops(c):
    need = 0
    for m in RD.finditer(c): need = max(need, int(m.group(1) or m.group(2)) + 1)
    pops = c.count('fp_pop()'); pushes = c.count('fp_push(') + c.count('fp_push_i64(')
    return max(need, pops), pushes - pops
def guess_icall(st, i):
    avail = 0
    for k, c in st[i+1:]:
        if k == 'label': return 0
        if CALL.search(c) or ICALL.search(c) or 'RECOMP_ITAIL' in c or 'RECOMP_RETURN' in c: return 0
        need, dd = ops(c)
        if need > avail: return 1
        avail += dd
        if c.startswith('goto ') or GOTO.search(c): return 0
    return 0
def analyze(name, deltas):
    body = funcs[name]; st = parse(name)
    has_fp = ('fp_push' in body) or ('fp_pop()' in body)
    bad = [b for b in BAD if b in body]
    labels = {d: i for i, (k, d) in enumerate(st) if k == 'label'}
    icall_idx = [i for i, (k, c) in enumerate(st) if k == 'stmt' and ICALL.search(c)]
    icd = {i: guess_icall(st, i) for i in icall_idx}
    depth = [None] * (len(st) + 1)
    work = [(0, 0)]; ret = set(); err = None; maxpush = 0; minpop = 0; unknown = False; maxacc = -99
    dyn_tail = False; ndefault = 0; guessed = {}
    while work:
        i, d = work.pop()
        while i < len(st):
            if depth[i] is not None:
                if depth[i] != d: err = 'merge'
                break
            depth[i] = d
            k, c = st[i]
            if k == 'label': i += 1; continue
            if 'RECOMP_ITAIL' in c:
                m = ITAILC.search(c)
                if m:
                    t = 'sub_' + m.group(1).upper(); dl = deltas.get(t)
                    if dl is None: unknown = True
                    else: ret.add(d + dl)
                elif 'default:' in c: ndefault += 1
                else: dyn_tail = True
                for g in GOTO.findall(c):   # case labels on the same line (switch)
                    if g in labels: work.append((labels[g], d))
                break
            for cm in CALL.finditer(c):
                dl = deltas.get(cm.group(1))
                if dl is None:
                    dl = guess_icall(st, i); guessed[i] = dl
                d += dl
            if i in icd: d += icd[i]
            need, dd = ops(c)
            maxacc = max(maxacc, need - d)
            d += dd
            maxpush = max(maxpush, d); minpop = min(minpop, d)
            if 'RECOMP_RETURN()' in c: ret.add(d); break
            for g in GOTO.findall(c):
                if g in labels: work.append((labels[g], d))
                else: err = 'goto outside'
            if c.startswith('goto ') or c.startswith('return'): break
            i += 1
    strict = bool(icall_idx or guessed)
    shape = not err and not unknown and len(ret) <= 1 and maxpush <= 8 and minpop >= -8 and maxacc <= 8
    if strict: shape = shape and minpop >= 0 and maxacc <= 0
    ok = shape and not bad
    delta = (sorted(ret)[0] if ret else 0) if shape and not dyn_tail else None
    why = ('bad:' + ','.join(bad)) if bad else err or ('unknown' if unknown else ('rets' if len(ret) > 1 else ('range' if not shape else 'ok')))
    return dict(fp=has_fp, lw=bool(ok and has_fp), delta=delta, k=0 if (minpop >= 0 and maxacc <= 0) else 8, icd=[icd[i] for i in icall_idx],
                cgs=[[st[i][1][st[i][1].find('sub_'):st[i][1].find('sub_') + 12], guessed[i]] for i in sorted(guessed)], why=why)
deltas = {}; info = {}
for it in range(20):
    changed = 0
    for n in funcs:
        r = analyze(n, deltas); info[n] = r
        if r['delta'] is not None and deltas.get(n) != r['delta']: deltas[n] = r['delta']; changed += 1
    print('pass', it, 'known deltas', len(deltas), 'changed', changed, file=sys.stderr)
    if not changed: break
json.dump(info, open(sys.argv[2], 'w'))
c = collections.Counter()
for n, r in info.items():
    if r['fp']: c['fp'] += 1; c['lw' if r['lw'] else r['why'].split(':')[0]] += 1
    if r['lw'] and r['icd']: c['lw with icall'] += 1
    if r['lw'] and any(r['icd']): c['lw with +1 icall'] += 1
    if r['lw'] and r['cgs']: c['lw with guessed direct calls'] += 1
print(dict(c), file=sys.stderr)
