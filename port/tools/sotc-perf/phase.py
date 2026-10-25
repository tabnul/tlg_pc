# Busy fraction per thread group vs. time since the last frame (flip), folded over frames in [a,b) s.
# phase.py samples log exe a b
import sys, runpy, bisect, collections, re
smp, log, exe, a, b = sys.argv[1], sys.argv[2], sys.argv[3], float(sys.argv[4]), float(sys.argv[5])
sys.argv = ['spike_report.py', smp, log, exe, '--each', '0', '--ms', '100000']
g = runpy.run_path(r'F:\development\sotc-oussama\tools\spike_report.py', run_name='lib')
samples, names, label, t0 = g['samples'], g['names'], g['label'], g['t0']
fr = [int(m.group(1)) for l in open(log, errors='ignore') for m in [re.match(r'SOTCFRAME \d+ t=\d+ms us=(\d+)', l)] if m]
fr = [t0 + x for x in fr]
groups = {'draw': 'GpuDraw', 'cp': 'GpuCommandProc', 'rec': 'GpuRecorder', 'update': 'Shadow Game Upd', 'render': 'Game Render', 'flip': 'RenderFlip', 'jobs': 'JobWorker'}
WAIT = ('ntdll', 'KERNELBASE', 'CpuRelax', 'win32u', 'nvoglv64')
B = 2000  # us
busy = collections.defaultdict(lambda: collections.Counter()); tot = collections.defaultdict(lambda: collections.Counter())
lens = []
for us, t, st in samples:
    if not (t0 + a * 1e6 <= us < t0 + b * 1e6): continue
    j = bisect.bisect_right(fr, us) - 1
    if j < 0 or j + 1 >= len(fr): continue
    n = names.get(t, '')
    grp = next((k for k, v in groups.items() if v in n), None)
    if not grp: continue
    ph = (us - fr[j]) // B
    leaf = label(st[0]) if st else '?'
    tot[grp][ph] += 1
    if not any(w in leaf for w in WAIT): busy[grp][ph] += 1
d = [y - x for x, y in zip(fr, fr[1:]) if t0 + a * 1e6 <= x < t0 + b * 1e6]
print('frames %d, median %.1f ms' % (len(d), sorted(d)[len(d) // 2] / 1000))
print('ms   ' + ' '.join('%5s' % k for k in groups))
for ph in range(16):
    print('%2d-%2d' % (ph * 2, ph * 2 + 2) + ' '.join('%5s' % ('%3.0f%%' % (100 * busy[k][ph] / tot[k][ph]) if tot[k][ph] else '-') for k in groups),
          '  n', tot['draw'][ph])
if __import__('os').environ.get('NAMES'):
    c = collections.Counter(names.get(t, '?') for us, t, st in samples if t0 + a * 1e6 <= us < t0 + b * 1e6)
    for k, v in c.most_common(40): print(v, k)
