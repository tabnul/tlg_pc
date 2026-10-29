# Scene check

Visits every start point of the game by itself, measures the FPS of each second and takes a screenshot
per point. The full guide is in [docs/scene-check.md](../../../docs/scene-check.md).

```
python scene_check.py LABEL --install DIR
python report.py LABEL [--base OTHER]
```

- `DIR` is the game folder with "Play The Last Guardian.exe".
- Needs Python 3.8 or newer with Pillow (`pip install pillow`).
- `scenes.json` is the list of points the check visits. `points.py` builds it again from your own game files.
- `start.save` is the save it starts from.

The start points come from the game's own QA tools, made reachable again by the GoldHEN
"Level Select + QA Menu" patch by @illusionyy and @DanielSvoboda. Thank you both.
