"""Results page of a scene check run, or of a run against a base run (same points, e.g. before and after a change).

  python report.py RUN [--base RUN] [--note TEXT]

Writes port/out/scene-check/RUN/index.html (with --base: RUN/vs-BASE.html) and 960 px thumbnails in each run's thumbs/.
The page: the problem list (every point whose status is not ok or that has a warning), a sortable table of the
points (10 s FPS, lowest and highest second, the change against the base, shader compiles, FPS per second) and
the screenshots, side by side with --base. Open it in a browser; it needs nothing online."""
import argparse, csv, html, json, os, statistics

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", "..", "out", "scene-check"))
ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
ap.add_argument("run")
ap.add_argument("--base")
ap.add_argument("--note", default="")
A = ap.parse_args()
E = html.escape
RUNS = [r for r in (A.base, A.run) if r]  # base first: left column, first series


def load(run):
    d = os.path.join(ROOT, run)
    rows = {f"{int(r['k']):03d}_{r['stage']}_{r['area']}": r for r in csv.DictReader(open(os.path.join(d, "results.csv")))}
    secs = json.load(open(os.path.join(d, "seconds.json"))) if os.path.exists(os.path.join(d, "seconds.json")) else {}
    os.makedirs(os.path.join(d, "thumbs"), exist_ok=True)
    return rows, secs


def num(r, k):
    try:
        return float(r.get(k) or "nan")
    except (ValueError, AttributeError):
        return float("nan")


def thumb(run, tag):
    """The shot as a 960 px JPEG in the run's thumbs/; None without a shot."""
    src = os.path.join(ROOT, run, "shots", tag + ".png")
    dst = os.path.join(ROOT, run, "thumbs", tag + ".jpg")
    if not os.path.exists(src):
        return None
    if not os.path.exists(dst):
        from PIL import Image
        im = Image.open(src).convert("RGB")
        im.thumbnail((960, 960))
        im.save(dst, quality=82)
    return dst


def vdiff(tag):
    """Mean grey difference of the two shots at 96x40 (0-255): a rough pointer to scenes that look different."""
    from PIL import Image, ImageChops, ImageStat
    pa, pb = (os.path.join(ROOT, r, "thumbs", tag + ".jpg") for r in RUNS)
    if not (os.path.exists(pa) and os.path.exists(pb)):
        return None
    a, b = (Image.open(p).convert("L").resize((96, 40)) for p in (pa, pb))
    return round(ImageStat.Stat(ImageChops.difference(a, b)).mean[0], 1)


def spark(series):
    """The 10 s of FPS of each run on one y axis per row, from the row's lowest to its highest second."""
    vals = [s[0] for ss in series for s in ss] or [1]
    lo, hi = min(vals) * 0.9, max(vals) * 1.05
    w, h, pad = 150, 36, 3
    out = []
    for n, (run, ss) in enumerate(zip(RUNS, series), 1):
        if not ss:
            continue
        step = (w - 2 * pad) / max(len(ss) - 1, 1)
        pts = [(pad + i * step, h - pad - (s[0] - lo) / max(hi - lo, 1) * (h - 2 * pad)) for i, s in enumerate(ss)]
        out.append(f'<polyline class="s{n}" points="' + " ".join(f"{x:.1f},{y:.1f}" for x, y in pts) + '"/>')
        out += [f'<circle cx="{x:.1f}" cy="{y:.1f}" r="5" class="hit"><title>{E(run)}, second {i + 1}: {s[0]:.0f} FPS, '
                f'worst frame {s[1]:.0f} ms, {s[2]} compiles</title></circle>' for i, ((x, y), s) in enumerate(zip(pts, ss))]
    return (f'<svg class="spark" viewBox="0 0 {w} {h}" width="{w}" height="{h}" role="img" aria-label="FPS per second">'
            f'<line class="base" x1="0" y1="{h - pad}" x2="{w}" y2="{h - pad}"/>{"".join(out)}</svg>')


def main():
    data = [load(r) for r in RUNS]
    tags = sorted(set().union(*(rows for rows, _ in data)), key=lambda t: int(t[:3]))
    problems, scenes, diffs = [], [], []
    for tag in tags:
        rs = [rows.get(tag, {}) for rows, _ in data]
        for run, r in zip(RUNS, rs):
            if r and (r.get("status") != "ok" or r.get("warn")):
                problems.append((tag, r, run))
        shots = [thumb(run, tag) for run in RUNS]
        vd = vdiff(tag) if A.base and all(shots) else None
        d = None
        if A.base:
            fb, ft = num(rs[0], "fps"), num(rs[1], "fps")
            if fb == fb and ft == ft and fb:
                d = (ft / fb - 1) * 100
                if all(r.get("status") == "ok" for r in rs):
                    diffs.append(d)
        scenes.append((tag, rs, d, vd))
    ok = [[num(rs[i], "fps") for _, rs, _, _ in scenes if rs[i].get("status") == "ok"] for i in range(len(RUNS))]

    def tile(label, value, sub=""):
        return f'<div class="tile"><div class="tl">{E(label)}</div><div class="tv">{value}</div><div class="ts">{E(sub)}</div></div>'
    tiles = [tile("Start points", len(tags), ", ".join(f"{len(o)} ok in {r}" for r, o in zip(RUNS, ok))),
             tile("Problems", len({p[0] for p in problems}), "points with a status other than ok or a warning")]
    tiles += [tile(f"{r} FPS", f"{statistics.mean(o):.1f}" if o else "-", "mean of the points' 10 s averages") for r, o in zip(RUNS, ok)]
    if diffs:
        tiles.append(tile("Change per point", f"{statistics.median(diffs):+.1f}%", f"median; {sum(d > 3 for d in diffs)} points > +3%, "
                          f"{sum(d < -3 for d in diffs)} < -3% (one visit each: ~3% is noise)"))
    title = f"Scene check: {A.run}" + (f" vs {A.base}" if A.base else "")
    parts = [f"<h1>{E(title)}</h1><p class='sub'>{E(A.note)}</p>", "<div class='tiles'>" + "".join(tiles) + "</div>",
             "<h2 id='problems'>Problem list</h2>"]
    if problems:
        parts.append("<div class='scroll'><table><thead><tr><th>#</th><th>stage</th><th>start point</th><th>run</th><th>status</th>"
                     "<th>warning</th><th>missions seen</th></tr></thead><tbody>" + "".join(
                         f"<tr><td>{tag[:3]}</td><td>{E(r['stage'])}</td><td>{E(r['area'])}</td><td>{E(run)}</td>"
                         f"<td><span class='st {'bad' if r['status'] != 'ok' else 'warn'}'>{E(r['status'])}</span></td><td>{E(r.get('warn', ''))}</td>"
                         f"<td class='small'>{E(r.get('missions', '')[:160])}</td></tr>" for tag, r, run in problems) + "</tbody></table></div>")
    else:
        parts.append("<p>None.</p>")
    legend = "<div class='legend'>" + "".join(f"<span><i class='k{n}'></i>{E(r)}</span>" for n, r in enumerate(RUNS, 1)) + "</div>"
    parts.append("<h2 id='points'>Start points</h2>" + legend + "<p class='small'>Click a header to sort. FPS: the 10 s average "
                 "(lowest-highest second). Compiles: shader compiles in the 10 s. Hover a dot for that second.</p>")
    head = ["#", "stage", "start point", "mission scene"] + [f"{r} FPS" for r in RUNS]
    head += ["change %", "compiles", "visual diff"] if A.base else ["compiles"]
    head.append("per second")
    body = []
    for tag, rs, d, vd in scenes:
        any_r = next(r for r in rs if r)
        fps = lambda x: f"{x['fps']} <span class='small'>({x['fps_min']}-{x['fps_max']})</span>" if x.get("fps") else E(x.get("status", ""))
        cells = [tag[:3], E(any_r["stage"]), E(any_r["area"]), f"<span class='small'>{E(any_r.get('scene', ''))}</span>"]
        cells = [f"<td>{c}</td>" for c in cells] + [f"<td data-v='{num(r, 'fps')}'>{fps(r)}</td>" for r in rs]
        if A.base:
            cells.append(f"<td data-v='{'' if d is None else d}'>{'' if d is None else f'{d:+.1f}'}</td>")
        cells.append("<td>" + "/".join(E(r.get("compiles", "")) for r in rs) + "</td>")
        if A.base:
            cells.append(f"<td>{'' if vd is None else vd}</td>")
        cells.append(f"<td>{spark([secs.get(tag, []) for _, secs in data])}</td>")
        body.append("<tr>" + "".join(cells) + "</tr>")
    parts.append("<div class='scroll'><table class='sortable'><thead><tr>" + "".join(f"<th>{h}</th>" for h in head)
                 + "</tr></thead><tbody>" + "".join(body) + "</tbody></table></div>")
    buttons = "<button data-f='all' class='on'>all</button><button data-f='issue'>problems</button>"
    if A.base:
        buttons += "<button data-f='diff'>largest visual difference first</button>"
    parts.append("<h2 id='shots'>Screenshots at 10 s</h2><p class='small'>" + (f"Left {E(A.base)}, right {E(A.run)}. " if A.base else "")
                 + "Click a picture for the full shot. The scenes are not deterministic (Trico, animals and the camera move), so a "
                 "difference is not a bug by itself; look for wrong colours, missing or stretched geometry, black or flickering "
                 f"surfaces.</p><div class='filters'>{buttons}</div><div id='shotlist'>")
    probtags = {p[0] for p in problems}
    for tag, rs, d, vd in scenes:
        any_r = next(r for r in rs if r)
        cells = []
        for run, r in zip(RUNS, rs):
            th = os.path.join(ROOT, run, "thumbs", tag + ".jpg")
            rel = f"../{run}/" if A.base else ""  # vs-BASE.html sits in RUN/ and reaches the base run next to it
            cells.append(f"<figure><a href='{rel}shots/{tag}.png'><img loading='lazy' src='{rel}thumbs/{tag}.jpg' alt='{E(tag)} {E(run)}'></a>"
                         f"<figcaption>{E(run)}: {E(r.get('status', '-'))}, {E(r.get('fps', '-'))} FPS</figcaption></figure>"
                         if os.path.exists(th) else f"<figure class='none'>{E(run)}: no shot ({E(r.get('status', '-'))})</figure>")
        parts.append(f"<section class='pair' data-issue='{int(tag in probtags)}' data-diff='{vd or 0}' data-k='{tag[:3]}'><h4>{tag[:3]} "
                     f"{E(any_r['stage'])} {E(any_r['area'])} <span class='small'>{E(any_r.get('scene', ''))}"
                     f"{'' if vd is None else f', visual diff {vd}'}</span></h4><div class='{'two' if A.base else 'one'}'>{''.join(cells)}</div></section>")
    parts.append("</div>")
    out = os.path.join(ROOT, A.run, f"vs-{A.base}.html" if A.base else "index.html")
    with open(out, "w", encoding="utf-8") as f:
        f.write(PAGE.replace("TITLE", E(title)).replace("BODY", "\n".join(parts)))
    print(out)


PAGE = """<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>TITLE</title><style>
:root{--bg:#fcfcfb;--fg:#1b1b1a;--muted:#62625e;--line:#e3e3df;--card:#f3f3f1;--s1:#2a78d6;--s2:#eb6834;--bad:#d03b3b;--warn:#9a6a00}
@media (prefers-color-scheme:dark){:root:not([data-theme=light]){--bg:#1a1a19;--fg:#ececea;--muted:#a3a39e;--line:#33332f;--card:#232321;--s1:#3987e5;--s2:#d95926;--bad:#e05a5a;--warn:#fab219}}
:root[data-theme=dark]{--bg:#1a1a19;--fg:#ececea;--muted:#a3a39e;--line:#33332f;--card:#232321;--s1:#3987e5;--s2:#d95926;--bad:#e05a5a;--warn:#fab219}
body{margin:0;padding:24px 16px;background:var(--bg);color:var(--fg);font:14px/1.45 system-ui,sans-serif;max-width:1500px;margin:auto}
h1{font-size:22px;margin:0 0 4px}h2{font-size:17px;margin:32px 0 8px;border-bottom:1px solid var(--line);padding-bottom:4px}
h4{font-size:13px;margin:0 0 6px;font-weight:600}.sub,.small{color:var(--muted)}.small{font-size:12px}
.tiles{display:flex;flex-wrap:wrap;gap:10px;margin:16px 0}.tile{background:var(--card);border-radius:8px;padding:10px 14px;min-width:150px}
.tl{font-size:12px;color:var(--muted)}.tv{font-size:24px;font-weight:600;font-variant-numeric:tabular-nums}.ts{font-size:11px;color:var(--muted);max-width:220px}
.scroll{overflow-x:auto}table{border-collapse:collapse;font-variant-numeric:tabular-nums;font-size:13px}th,td{padding:4px 8px;border-bottom:1px solid var(--line);text-align:left;vertical-align:middle;white-space:nowrap}
td.small{white-space:normal;min-width:200px}th{color:var(--muted);font-weight:600;position:sticky;top:0;background:var(--bg)}.sortable th{cursor:pointer}
.st{font-weight:600}.st.bad{color:var(--bad)}.st.warn{color:var(--warn)}
.legend{display:flex;gap:16px;font-size:12px;color:var(--muted);margin:4px 0}.legend i{display:inline-block;width:14px;height:2px;vertical-align:middle;margin-right:6px}
.k1{background:var(--s1)}.k2{background:var(--s2)}
.spark polyline{fill:none;stroke-width:2;stroke-linejoin:round}.spark .s1{stroke:var(--s1)}.spark .s2{stroke:var(--s2)}.spark .base{stroke:var(--line)}
.spark .hit{fill:transparent}.spark .hit:hover{fill:var(--fg)}
.filters{display:flex;flex-wrap:wrap;gap:6px;margin:8px 0 12px}.filters button{background:var(--card);color:var(--fg);border:1px solid var(--line);border-radius:6px;padding:4px 10px;cursor:pointer}
.filters button.on{border-color:var(--fg)}.pair{margin:0 0 18px}.two{display:grid;grid-template-columns:1fr 1fr;gap:8px}.one{max-width:760px}
figure{margin:0}figure img{width:100%;height:auto;border-radius:4px;display:block}figcaption{font-size:12px;color:var(--muted)}
figure.none{background:var(--card);border-radius:4px;padding:20px;color:var(--muted)}
@media (max-width:700px){.two{grid-template-columns:1fr}}
</style></head><body>
BODY
<script>
document.querySelectorAll('table.sortable th').forEach((th,i)=>th.addEventListener('click',()=>{
 const tb=th.closest('table').tBodies[0],rows=[...tb.rows],dir=th.dataset.dir=th.dataset.dir==='a'?'d':'a';
 const val=r=>{const c=r.cells[i],v=c.dataset.v??c.textContent.trim();const n=parseFloat(v);return isNaN(n)?v:n};
 rows.sort((a,b)=>{const x=val(a),y=val(b);const r=typeof x==='number'&&typeof y==='number'?x-y:String(x).localeCompare(String(y));return dir==='a'?r:-r});
 rows.forEach(r=>tb.appendChild(r))}));
document.querySelectorAll('.filters button').forEach(b=>b.addEventListener('click',()=>{
 document.querySelectorAll('.filters button').forEach(x=>x.classList.toggle('on',x===b));
 const list=document.getElementById('shotlist'),pairs=[...list.children];
 pairs.forEach(p=>p.style.display=b.dataset.f==='issue'&&p.dataset.issue!=='1'?'none':'');
 const key=b.dataset.f==='diff'?p=>-parseFloat(p.dataset.diff):p=>parseInt(p.dataset.k,10);
 pairs.sort((x,y)=>key(x)-key(y)).forEach(p=>list.appendChild(p))}));
</script></body></html>"""

main()
