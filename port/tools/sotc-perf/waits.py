# What threads of a group wait on (first frame outside system DLLs) by phase since the last present.
# waits.py samples log exe a b group-substr
import sys, runpy, bisect, collections, re
smp, log, exe, a, b, grp = sys.argv[1], sys.argv[2], sys.argv[3], float(sys.argv[4]), float(sys.argv[5]), sys.argv[6]
sys.argv = ['spike_report.py', smp, log, exe, '--each', '0', '--ms', '100000']
g = runpy.run_path(r'F:\development\sotc-oussama\tools\spike_report.py', run_name='lib')
samples, names, label, t0 = g['samples'], g['names'], g['label'], g['t0']
fr = [t0 + int(m.group(1)) for l in open(log, errors='ignore') for m in [re.match(r'SOTCFRAME \d+ t=\d+ms us=(\d+)', l)] if m]
SYS = ('ntdll', 'KERNELBASE', 'KERNEL32', 'ucrtbase', 'libc', 'win32u', 'pthread_cond', 'pthread_mutex', 'std::')
c = collections.defaultdict(collections.Counter)
for us, t, st in samples:
    if not (t0 + a * 1e6 <= us < t0 + b * 1e6) or grp not in names.get(t, ''): continue
    j = bisect.bisect_right(fr, us) - 1
    if j < 0: continue
    ph = min((us - fr[j]) // 4000, 5)
    labs = [label(x) for x in st]
    k = next((l for l in labs if not any(s in l for s in SYS)), '?')
    if not any(s in labs[0] for s in ('ntdll', 'KERNELBASE')): k = 'BUSY'
    c[ph][k[:60]] += 1
for ph in sorted(c):
    tot = sum(c[ph].values())
    print('%2d-%2d ms:' % (ph * 4, ph * 4 + 4), ', '.join('%s %.0f%%' % (k, 100 * v / tot) for k, v in c[ph].most_common(5)))
