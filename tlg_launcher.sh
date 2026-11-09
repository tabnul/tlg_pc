#!/usr/bin/env bash
# Opens The Last Guardian launcher (settings, language, play).
exec python3 "$(dirname -- "$(realpath -- "$0")")/port/launcher/tlg_launcher.py" "$@"
