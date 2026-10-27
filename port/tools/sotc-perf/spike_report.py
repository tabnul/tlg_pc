#!/usr/bin/env python3
"""sotc: what each thread did during slow frames, from a SOTC_SPIKE_OUT sample file and the run log
(SOTC_FRAME_LOG=1 for every frame's time, SOTCCLOCK line for the clock):
spike_report.py samples.bin last_run.log [exe] [--ms 40] [--top 12] [--each 8]
Per thread: frames (inclusive) over-represented in slow frames vs. normal frames, then the
slowest frames one by one with each busy thread's top stacks."""
import argparse, bisect, collections, re, struct, subprocess, sys
from pathlib import Path

p = argparse.ArgumentParser()
p.add_argument('samples'); p.add_argument('log'); p.add_argument('exe', nargs='?', default='out/gpu/bb-probe.exe')
p.add_argument('--ms', type=float, default=40.0, help='slow frame threshold')
p.add_argument('--top', type=int, default=12); p.add_argument('--each', type=int, default=8)
p.add_argument('--threads', default='', help='only threads whose name contains one of these (comma list)')
a = p.parse_args()

data = Path(a.samples).read_bytes()
mods, names, samples, image = {}, {}, [], 0
i = 0
while i < len(data):
    k = data[i]; i += 1
    try:
        if k == 0x47:  # G
            image, = struct.unpack_from('<Q', data, i); i += 8
        elif k == 0x4d:  # M
            b, s, n = struct.unpack_from('<QIH', data, i); i += 14
            mods[b] = (s, data[i:i + n].decode(errors='replace')); i += n
        elif k == 0x54:  # T
            t, n = struct.unpack_from('<IH', data, i); i += 6
            names[t] = data[i:i + n].decode(errors='replace'); i += n
        elif k == 0x53:  # S
            us, t, n = struct.unpack_from('<QIB', data, i); i += 13
            samples.append((us, t, struct.unpack_from(f'<{n}Q', data, i))); i += 8 * n
        else:
            break
    except struct.error:
        break  # file cut while the game was running
samples.sort()
times = [s[0] for s in samples]
bases = sorted(mods)
exe_name = Path(a.exe).name.lower()


def where(addr):
    if image and 0 <= addr - image < 0x20000000:
        return ('guest', addr - image)
    j = bisect.bisect_right(bases, addr) - 1
    if j >= 0 and addr - bases[j] < mods[bases[j]][0]:
        return (mods[bases[j]][1], addr - bases[j])
    return ('?', addr)


# exe offsets -> function names (one llvm-symbolizer run)
exe_offsets = set()
for _, _, st in samples:
    for addr in st:
        m, off = where(addr)
        if m.lower() == exe_name:
            exe_offsets.add(off)
symbols = {}
if exe_offsets and Path(a.exe).exists():
    offs = sorted(exe_offsets)
    try:
        r = subprocess.run(['llvm-symbolizer', f'--obj={a.exe}', '--functions=short', '--no-inlines', '--demangle'],
                           input='\n'.join(hex(0x140000000 + o) for o in offs), capture_output=True, text=True)
        out = [blk.split('\n')[0] for blk in r.stdout.strip().split('\n\n')]
        symbols = dict(zip(offs, out))
    except FileNotFoundError:
        print('(llvm-symbolizer not found: exe frames stay offsets)', file=sys.stderr)


def label(addr):
    m, off = where(addr)
    if m.lower() == exe_name:
        s = symbols.get(off)
        return s if s and s != '??' else f'exe+0x{off:x}'
    if m == 'guest':
        return f'guest+0x{off:x}'
    return f'{m}+0x{off:x}' if m != '?' else hex(off)


def func(addr):
    """Frames aggregated per function for exe code, per module for system DLLs, raw for guest."""
    m, off = where(addr)
    if m.lower() == exe_name or m == 'guest':
        return label(addr)
    return m


log = Path(a.log).read_text(encoding='utf-8', errors='replace')
c = re.search(r'SOTCCLOCK t0=(\d+)', log)
if not c:
    sys.exit('no SOTCCLOCK line (run with SOTC_FRAME_LOG=1 and the spike build)')
t0 = int(c.group(1))
ends = [t0 + int(m.group(1)) for m in re.finditer(r'SOTCFRAME \d+ t=\d+ms us=(\d+)', log)]
stalls = {int(m.group(1)): m.group(0) for m in re.finditer(r'SOTCSTALL frame (\d+) .*', log)}
frames = [(ends[j - 1], ends[j], j + 1) for j in range(1, len(ends))]
if not frames or not samples:
    sys.exit(f'{len(frames)} frames, {len(samples)} samples')
print(f'{len(samples)} samples, {len(names)} threads, {len(frames)} frames; '
      f'samples {(times[0] - t0) / 1e6:.1f}..{(times[-1] - t0) / 1e6:.1f} s, frames ..{(ends[-1] - t0) / 1e6:.1f} s')

want = [w for w in a.threads.split(',') if w]
slow = [f for f in frames if (f[1] - f[0]) / 1000 >= a.ms]
durs = sorted((f[1] - f[0]) / 1000 for f in frames)
print(f'frame ms: median {durs[len(durs) // 2]:.1f}, p99 {durs[int(len(durs) * .99)]:.1f}, max {durs[-1]:.1f}; '
      f'{len(slow)} frames >= {a.ms:g} ms')


def window(lo, hi):
    return samples[bisect.bisect_left(times, lo):bisect.bisect_left(times, hi)]


slow_set = collections.Counter(); base_set = collections.Counter()
slow_n = collections.Counter(); base_n = collections.Counter()
mark = [False] * len(samples)
for lo, hi, _ in slow:
    for j in range(bisect.bisect_left(times, lo), bisect.bisect_left(times, hi)):
        mark[j] = True
for j, (_, t, st) in enumerate(samples):
    name = names.get(t, str(t))
    if want and not any(w in name for w in want):
        continue
    fs = set(func(x) for x in st)
    (slow_n if mark[j] else base_n)[name] += 1
    for f in fs:
        (slow_set if mark[j] else base_set)[(name, f)] += 1

print('\n== over-represented in slow frames (inclusive %, slow vs normal) ==')
for name in sorted(slow_n, key=lambda n: -slow_n[n]):
    rows = []
    for (n, f), cnt in slow_set.items():
        if n != name:
            continue
        ps = cnt / slow_n[name]; pb = base_set[(n, f)] / base_n[name] if base_n[name] else 0
        if ps - pb >= 0.05:
            rows.append((ps - pb, ps, pb, f))
    if not rows:
        continue
    print(f'-- {name} ({slow_n[name]} slow / {base_n[name]} normal samples)')
    for d, ps, pb, f in sorted(rows, reverse=True)[:a.top]:
        print(f'   {100 * ps:5.1f}% vs {100 * pb:5.1f}%  {f}')

print(f'\n== slowest {a.each} frames ==')
for lo, hi, n in sorted(slow, key=lambda f: f[0] - f[1])[:a.each]:
    print(f'\n# frame {n} at {(hi - t0) / 1e6:.2f} s: {(hi - lo) / 1000:.0f} ms')
    if n in stalls:
        print('  ' + stalls[n])
    per = collections.defaultdict(collections.Counter)
    for _, t, st in window(lo, hi):
        name = names.get(t, str(t))
        if want and not any(w in name for w in want):
            continue
        # leaf..caller chain, system frames collapsed to their module
        chain = []
        for x in st[:10]:
            f = func(x)
            if not chain or chain[-1] != f:
                chain.append(f)
        per[name][' < '.join(chain[:6])] += 1
    for name, stacks in sorted(per.items(), key=lambda kv: -sum(kv[1].values())):
        total = sum(stacks.values())
        top = stacks.most_common(2)
        # threads that only wait the whole frame are noise
        if len(stacks) == 1 and re.search(r'ntdll|win32u|KERNELBASE', top[0][0].split(' < ')[0]) and total < 3:
            continue
        print(f'  {name} ({total}):')
        for s, cnt in top:
            print(f'     {100 * cnt / total:3.0f}% {s}')
