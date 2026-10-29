---
name: scene-check
description: Run the scene check in The Last Guardian port. It visits every start point of the game (or a list) and measures the FPS of each, compares two runs, or opens scenes one by one for the user to look at. Use when the user wants to test a change across the whole game, find the slowest scenes, or look at specific scenes.
---

Read `docs/scene-check.md` first: it has the commands, the options, the statuses and how much a
difference means. Then:

1. Before a run, tell the user that a game window opens and that the run is hands off (the game
   takes the focus for every screenshot). Say how long it takes: about 40 s per point, about
   2.5 hours for the whole game. Run it in the background, and don't build or start the game
   while it runs.
2. The tool sets no logos, no HDR, unlocked FPS, Fullscreen and no texture pack itself. Leave
   diagnostic environment variables off, because they cost FPS.
3. For an A/B, give both runs the same settings. Only the change differs, by `--exe` or `--env`.
   Then run `report.py NEW --base OLD`. Judge the whole run and the stage averages: a single
   scene moves 3 to 8% between two runs of the same build.
4. Treat a status as a hint. Before you call a scene broken, look at its screenshot, then open it
   with `--seconds 0` and let the user judge it. Known cases that are not the scene's fault:
   - A rare crash at the start of a load (divide by zero at guest `0x149a1a0`, issue #19), about
     1 in 250 visits at random. Run that point again.
   - Scenes that are dark or fade in late by design.
   - The ending scenes, which load twice or restart the game.
5. For a smaller test, give `--points` a list, for example the slowest scenes of an earlier run
   (sort its `results.csv` by `fps`).
6. Keep the output folders of earlier runs, because they are the baselines for later A/Bs. In
   anything written for others, name each scene by stage, start point and mission scene, as the
   report shows them.
