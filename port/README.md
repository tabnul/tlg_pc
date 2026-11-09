# port/ — loader, runtime and video core of The Last Guardian for PC

This folder holds the code that runs TLG's original `eboot.bin` (PS4, CUSA03627 v1.03) on PC:

- `scripts/`: prepares and links the eboot with the game's and the console's modules (`prepare.py`, `link_modules.py`);
- `src/`: the loader (`probe.c`) and the runtime that reimplements the PS4 libraries (`runtime_*.c`, `runtime_tlg.c`);
- `gpu/`: GPU translation to Vulkan (the shadPS4 core, plus the overlay/menu in `gpu/shim/`);
- `launcher/`: the graphical launcher (`tlg_launcher.py`) and the run core (`tlg_run.py`).

Documentation, status and usage are in the [main README](../README.md).
Where the code comes from (gow3_pc → bloodborne_pc, shadPS4) and the license (GPL-2.0-or-later) are in
[UPSTREAM.md](UPSTREAM.md) and [LICENSE](LICENSE).
