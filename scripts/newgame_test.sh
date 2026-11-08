#!/usr/bin/env bash
# Automated test: starts the game, waits for the main menu, picks New Game and watches for N seconds.
# Scripted input via TLG_PAD_FILE (runtime_pad.c): the file lists the buttons held down.
# Usage: scripts/newgame_test.sh [play_seconds=120] [menu_wait=45]
set -uo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
play=${1:-120}; menu_wait=${2:-45}
pad=$root/port/out/pad_input.txt
: > "$pad"
export TLG_PAD_FILE=$pad
press() { echo "$1" > "$pad"; sleep "${2:-0.25}"; : > "$pad"; sleep 0.6; echo "[$(date +%T)] pressed $1"; }
"$root/run_tlg.sh" $((menu_wait + play + 15)) > /dev/null 2>&1 &
game=$!
sleep "$menu_wait"
press up; press cross
sleep 6; press cross      # a possible confirmation ("start a new game?") or autosave notice
sleep 6; press cross
wait "$game"
echo "done (exit $?); log: $root/port/out/last_run.log"
