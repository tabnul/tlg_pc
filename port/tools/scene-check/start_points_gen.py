"""Writes the start point lists of the level jump from scenes.json (made by points.py): the in-game menu's Fun tab
(gpu/shim/tlg_fun_points.h) and the launcher's Start at... page (launcher/start_points.py). Run it after scenes.json
changes: python start_points_gen.py"""
import json, os

HERE = os.path.dirname(os.path.abspath(__file__))
PORT = os.path.dirname(os.path.dirname(HERE))
points = json.load(open(os.path.join(HERE, "scenes.json"), encoding="utf-8"))
note = "made by tools/scene-check/start_points_gen.py from scenes.json (same list and order as the scene check)"

c = ["// SPDX-License-Identifier: GPL-2.0-or-later",
     "// tlg: start points of the level jump (Fun tab, TLG_START_POINT): every StartAreas entry of the story",
     f"// stages; {note}.", "#pragma once", "", "namespace TlgFun {", "struct Point {", "    const char* stage;",
     "    const char* area;", "    const char* scene;", "};", "", "inline constexpr Point Points[] = {"]
c += [f'    {{"{p["stage"]}", "{p["area"]}", "{p["scene"]}"}},' for p in points]
c += ["};", "} // namespace TlgFun", ""]
with open(os.path.join(PORT, "gpu", "shim", "tlg_fun_points.h"), "w", encoding="utf-8", newline="\n") as f:
    f.write("\n".join(c))

py = ["# SPDX-License-Identifier: GPL-2.0-or-later", f'"""Start points of the launcher\'s Start at... page: (stage, area); {note}."""',
      "POINTS = ("]
py += [f'    ({p["stage"]!r}, {p["area"]!r}),' for p in points]
py += [")", ""]
with open(os.path.join(PORT, "launcher", "start_points.py"), "w", encoding="utf-8", newline="\n") as f:
    f.write("\n".join(py))
print(len(points), "points")
