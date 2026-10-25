# Inclusive/exclusive function profile of one thread in [a,b) s: samples log exe a b thread
import sys, runpy, collections, bisect, subprocess
a, b, want, exe = float(sys.argv[4]), float(sys.argv[5]), sys.argv[6], sys.argv[3]
sys.argv = ['spike_report.py', sys.argv[1], sys.argv[2], sys.argv[3], '--each', '0', '--ms', '100000']
g = runpy.run_path(r'F:\development\sotc-oussama\tools\spike_report.py', run_name='lib')
samples, times, names, label, t0 = g['samples'], g['times'], g['names'], g['label'], g['t0']
sel = [st for us, t, st in samples[bisect.bisect_left(times, t0 + a * 1e6):bisect.bisect_left(times, t0 + b * 1e6)]
       if want in names.get(t, '')]
labs = [[label(x) for x in st] for st in sel]
addrs = sorted({l for ls in labs for l in ls if l.startswith('bb-probe.exe+')})
out = subprocess.run(['llvm-addr2line', '-f', '-C', '-e', exe] + [hex(0x140000000 + int(x.split('+')[1], 16) - 1) for x in addrs],
                     capture_output=True, text=True).stdout.split('\n')
fn = {x: out[2 * i][:90] for i, x in enumerate(addrs)}
name = lambda l: fn.get(l, l.split('+')[0])
inc, exc = collections.Counter(), collections.Counter()
for ls in labs:
    ns = [name(l) for l in ls]
    exc[ns[0] if ns else '?'] += 1
    for n in set(ns): inc[n] += 1
N = len(labs); print(N, 'samples')
print('-- exclusive'); [print(f'{v*100/N:5.1f}% {k}') for k, v in exc.most_common(25)]
print('-- inclusive'); [print(f'{v*100/N:5.1f}% {k}') for k, v in inc.most_common(45)]
if len(sys.argv) > 0 and __import__('os').environ.get('FOCUS'):
    f = __import__('os').environ['FOCUS']
    kids, tot = collections.Counter(), 0
    for ls in labs:
        ns = [name(l) for l in ls]  # leaf first
        idx = next((i for i, n in enumerate(ns) if n.startswith(f)), None)
        if idx is None: continue
        tot += 1
        kids[ns[idx - 1] if idx > 0 else '<self>'] += 1
    print('-- callees of', f, tot)
    [print(f'{v*100/N:5.1f}% {k}') for k, v in kids.most_common(25)]
if __import__('os').environ.get('SYSCALLERS'):
    c, tot = collections.Counter(), 0
    for ls in labs:
        ns = [name(l) for l in ls]
        if not ns or not any(k in ns[0] for k in ('ntdll', 'ucrtbase', 'KERNEL', 'kernel')): continue
        tot += 1
        app = [n for n in ns if not any(k in n for k in ('ntdll', 'ucrtbase', 'KERNEL', 'kernel', 'libc++', 'operator new', 'operator delete', 'malloc', 'free'))]
        c[' <- '.join(a[:60] for a in app[:2]) if app else '<none>'] += 1
    print('-- sys callers', tot * 100 / N)
    [print(f'{v*100/N:5.1f}% {k}') for k, v in c.most_common(30)]
if __import__('os').environ.get('LIBCSTACKS'):
    c = collections.Counter()
    for ls in labs:
        ns = [name(l) for l in ls]
        if ns and 'ntdll' in ns[0]:
            c[' | '.join(n[:40] for n in ns[:10] if 'libc' != n)] += 1
    [print(f'{v*100/N:5.1f}% {k}') for k, v in c.most_common(12)]
if __import__('os').environ.get('EXCLINE'):
    # leaf-most own frame (skip system/libc++ frames) by source line
    c = collections.Counter()
    for ls in labs:
        own = next((l for l in ls if l.startswith('bb-probe.exe+')), None)
        if own: c[own] += 1
    top = c.most_common(60)
    o = subprocess.run(['llvm-addr2line', '-f', '-C', '-i', '-e', exe] + [hex(0x140000000 + int(x.split('+')[1], 16) - 1) for x, _ in top],
                       capture_output=True, text=True).stdout
    # with -i, inlined frames add lines; re-run per address for simplicity
    byline = collections.Counter()
    for x, v in top:
        r = subprocess.run(['llvm-addr2line', '-f', '-C', '-e', exe, hex(0x140000000 + int(x.split('+')[1], 16) - 1)], capture_output=True, text=True).stdout.split('\n')
        byline[(r[0][:50], r[1].split('shadps4/')[-1].split('sotc-oussama/')[-1])] += v
    print('-- own leaf lines (incl. time in system callees)')
    [print(f'{v*100/N:5.1f}% {k[1]}  {k[0]}') for k, v in byline.most_common(40)]
if __import__('os').environ.get('SITES'):
    f = __import__('os').environ['SITES']
    c = collections.Counter()
    for ls in labs:
        for l in ls:
            if name(l).startswith(f): c[l] += 1; break
    o = {}
    for x, v in c.most_common(80):
        r = subprocess.run(['llvm-addr2line', '-C', '-e', exe, hex(0x140000000 + int(x.split('+')[1], 16) - 1)], capture_output=True, text=True).stdout.strip()
        o[r.split('shadps4/')[-1]] = o.get(r.split('shadps4/')[-1], 0) + v
    for k, v in sorted(o.items(), key=lambda kv: -kv[1])[:40]: print(f'{v*100/N:5.1f}% {k}')
if __import__('os').environ.get('UP'):
    f = __import__('os').environ['UP']
    c = collections.Counter()
    for ls in labs:
        ns = [name(l) for l in ls]
        idx = next((i for i, n in enumerate(ns) if n.startswith(f)), None)
        if idx is None: continue
        c[' < '.join(ns[idx + 1:idx + 4])] += 1
    print('-- callers of', f)
    [print(f'{v*100/N:5.1f}% {k}') for k, v in c.most_common(15)]
