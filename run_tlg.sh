#!/usr/bin/env bash
# Runs The Last Guardian (CUSA03627 v1.03) through the native loader, without the launcher window.
# Usage: ./run_tlg.sh [seconds]   (default: no limit; close the window or press Ctrl+C to quit)
# Settings come from the launcher (~/.config/tlg-launcher/settings.json); TLG_* variables
# in the environment win (e.g. TLG_READBACKS=1 ./run_tlg.sh). See README.md.
# Graphical launcher: ./tlg_launcher.sh
exec python3 "$(dirname -- "$(realpath -- "$0")")/port/launcher/tlg_run.py" "$@"
