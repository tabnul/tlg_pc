"""Lists the game's start points for scene_check.py: every StartAreas entry in the stages' regionsetting.lua, read from
your own copy of the game (trico_boot.psarc; trico_bootp.psarc, the update, wins where it has the same file).

  python points.py [--game DIR]

--game: the game folder (CUSA03745 or CUSA03627). Defaults to the launcher's game folder (settings.json).
Writes scenes.json next to this script: [{stage, area, scene}] for the story stages s01-s18, without the entries the
game marks testOnly and the ones with no mission scene. The order is the check's order (k in results.csv). The tool
comes with this list (made from update 01.03), so you only need this after a game update changes the start points."""
import argparse, hashlib, json, lzma, os, re, struct, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "scenes.json")
SETTINGS = os.path.join(os.environ.get("APPDATA", ""), "tlg-launcher", "settings.json")
# scenes that are not a place to stand: the stage's bottom fallback and the opening movie
NOT_A_PLACE = ("Bottom", "OP_A_Start")


def psarc(path):
    """The archive's file names and a function that reads one by name."""
    f = open(path, "rb")
    magic, _, _, comp, toc_len, entry_size, count, block, _ = struct.unpack(">4sHH4sIIIII", f.read(32))
    if magic != b"PSAR":
        raise ValueError(f"{path}: not a PSARC archive")
    toc = f.read(toc_len - 32)
    entries = []
    for i in range(count):
        e = toc[i * entry_size:(i + 1) * entry_size]
        entries.append((struct.unpack(">I", e[16:20])[0], int.from_bytes(e[20:25], "big"), int.from_bytes(e[25:30], "big")))
    width = 2 if block <= 0x10000 else 3 if block <= 0x1000000 else 4
    rest = toc[count * entry_size:]
    blocks = [int.from_bytes(rest[i:i + width], "big") for i in range(0, len(rest) - width + 1, width)]

    def read(n):
        idx, size, off = entries[n]
        out = bytearray()
        f.seek(off)
        while len(out) < size:
            z = blocks[idx]
            want = min(block, size - len(out))
            data = f.read(z if z else block)
            if not data:
                raise ValueError(f"{path}: entry {n} runs past the end of the file")
            if z == 0 or z == want:
                out += data
            elif comp == b"zlib":
                out += zlib.decompress(data)
            else:
                out += lzma.LZMADecompressor(lzma.FORMAT_ALONE).decompress(data)
            idx += 1
        return bytes(out[:size])
    # The name list is newline or NUL separated; the update (trico_bootp) does not keep its entries in that order,
    # so each name's entry is found by its MD5 (of the upper-case name: the archives ignore case).
    md5 = {toc[i * entry_size:i * entry_size + 16]: i for i in range(count)}
    names = [n for n in re.split("[\n\0]", read(0).decode("utf-8", "replace")) if n]
    by_name = {n.lower(): md5[hashlib.md5(n.upper().encode()).digest()] for n in names}
    return by_name, lambda name: read(by_name[name])


def start_areas(src):
    """StartAreas.<stage> = { <area> = { ... scene = "...", testOnly = 1 ... }, ... } -> {stage: {area: (scene, test)}}"""
    src = re.sub(r"--\[\[.*?\]\]", "", src, flags=re.S)
    src = re.sub(r"--[^\n]*", "", src)
    out = {}
    for m in re.finditer(r"StartAreas\.(\w+)\s*=\s*\{", src):
        entries = out.setdefault(m[1], {})
        i, depth, key, start = m.end(), 1, None, None
        while depth and i < len(src):
            if src[i] == "{":
                if depth == 1:
                    k = re.search(r"(\w+)\s*=\s*$", src[max(0, i - 80):i])
                    key, start = (k[1] if k else None), i
                depth += 1
            elif src[i] == "}":
                depth -= 1
                if depth == 1 and key:
                    body = src[start:i]
                    test = re.search(r"testOnly\s*=\s*(\d+)", body)
                    scene = re.search(r"""scene\s*=\s*["']([^"']*)["']""", body)
                    entries[key] = (scene[1] if scene else "", bool(test and test[1] != "0"))
                    key = None
            i += 1
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--game")
    A = ap.parse_args()
    game = A.game or json.load(open(SETTINGS, encoding="utf-8")).get("game_dir")
    if not game or not os.path.exists(os.path.join(game, "trico_boot.psarc")):
        ap.error("--game: the game folder with trico_boot.psarc")
    files = {}
    for name in ("trico_boot.psarc", "trico_bootp.psarc"):  # the update last, so its files win
        path = os.path.join(game, name)
        if not os.path.exists(path):
            continue
        names, read = psarc(path)
        found = [n for n in names if n.endswith("/regionsetting.lua")]
        for n in found:
            files[n] = (name, read(n).decode("utf-8", "replace"))
        print(f"{name}: {len(found)} regionsetting.lua")
    areas = {}
    for n, (_, src) in sorted(files.items()):
        for stage, entries in start_areas(src).items():
            areas.setdefault(stage, {}).update(entries)
    points = [dict(stage=stage, area=area, scene=scene) for stage in sorted(areas) if re.match(r"s(0[1-9]|1[0-8])_", stage)
              for area, (scene, test) in sorted(areas[stage].items()) if not test and scene and scene not in NOT_A_PLACE]
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    json.dump(points, open(OUT, "w"), indent=1)
    print(f"{OUT}: {len(points)} start points in {len({p['stage'] for p in points})} stages")


main()
