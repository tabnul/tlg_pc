import re, sys
D = r'F:\development\sotc-oussama\out\routes'
for f in sys.argv[1:]:
    L = open(D + '\\' + f + '.route.txt', errors='ignore').read().splitlines()
    fps = [(float(m.group(2)), float(m.group(1))) for l in L
           for m in [re.match(r'SOTC FPS ([\d.]+) at (\d+)s', l)] if m]
    w = [v for t, v in fps if 60 <= t <= 210]
    rb = [l for l in L if l.startswith('SOTCRB')]
    g = sum(int(re.search(r' gpu=(\d+)', l).group(1)) for l in rb) / 1e6
    d1 = sum(1 for l in rb if ' dirty=1' in l)
    print(f, 'fps mean %.1f min %.1f secs<55 %d/%d' % (sum(w) / len(w), min(w), sum(v < 55 for v in w), len(w)),
          'rb', len(rb), 'gpu %.1fs dirty1 %d' % (g, d1))
