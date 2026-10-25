import re, sys
D = r'F:\development\sotc-oussama\out\routes'
for f in sys.argv[1:]:
    ts = [int(m.group(1)) for l in open(D + '\\' + f + '.route.txt', errors='ignore')
          for m in [re.match(r'SOTCFRAME \d+ t=\d+ms us=(\d+)', l)] if m]
    # gameplay: from 30 s into the run to the end
    ts = [t for t in ts if t >= 30e6]
    d = sorted((b - a) / 1000 for a, b in zip(ts, ts[1:]))
    n = len(d)
    secs = (ts[-1] - ts[0]) / 1e6
    print('%-5s %5.0fs frames %d  avg %.1f fps  p50 %.1f p99 %.1f  >20ms %d  >33ms %d  >50ms %d  worst %.0f ms' % (
        f, secs, n, n / secs, d[n // 2], d[int(n * .99)], sum(x > 20 for x in d), sum(x > 33 for x in d),
        sum(x > 50 for x in d), d[-1]))
