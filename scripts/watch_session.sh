#!/usr/bin/env bash
# Watches a game session: reports when it starts/closes/crashes and, if the game stops presenting frames
# for $STALL seconds (no "Frame stats" in the log), dumps the threads with kill -USR1.
# One line per event (made for the Claude Code Monitor). FPS summary every $SUMMARY s.
STALL=${STALL:-20}; SUMMARY=${SUMMARY:-120}
logs=$HOME/decomp/port/out/logs
pid=""; log=""; dumped=0; last_sum=$(date +%s)
while true; do
  p=$(pgrep -f "out/tlg-probe .*boot-linked.bin" | head -1)
  if [[ -n $p && $p != "$pid" ]]; then
    pid=$p; dumped=0; sleep 2; log=$(ls -1t "$logs"/run-*.log | head -1)
    echo "Session started (pid $pid, log $(basename "$log"))"
  fi
  if [[ -n $pid && -z $p ]]; then
    last=$(grep -E "STOP|fault \(signal|Assertion" "$log" | tail -1)
    echo "Session ended (pid $pid). ${last:-Closed normally.}"
    pid=""
  fi
  if [[ -n $pid ]]; then
    lastfs=$(grep -n "^Frame stats:" "$log" | tail -1)
    age=$(( $(date +%s) - $(stat -c %Y "$log") ))
    fs_line=${lastfs%%:*}; total=$(wc -l < "$log")
    # no "Frame stats" in the last lines and the log idle for STALL s => hung
    if [[ -n $lastfs ]] && (( dumped == 0 )); then
      since=$(tail -n +"$fs_line" "$log" | grep -c "^Frame stats:")
      lastfs_age=$(( $(date +%s) - $(stat -c %Y "$log") ))
      newer=$(awk -v n="$fs_line" 'NR>n' "$log" | grep -c "^Frame stats:")
    fi
    if [[ -n $lastfs ]] && (( dumped == 0 )); then
      ts_file=/tmp/.tlg_fs_$pid
      cur=$(grep -c "^Frame stats:" "$log")
      prev=$(cat "$ts_file" 2>/dev/null || echo "0 $(date +%s)")
      read pc pt <<<"$prev"
      if (( cur != pc )); then echo "$cur $(date +%s)" > "$ts_file"
      elif (( $(date +%s) - pt >= STALL )); then
        kill -USR1 "$pid" 2>/dev/null; dumped=1
        echo "HUNG: ${STALL}s without frames (last: $(grep '^Frame stats:' "$log" | tail -1 | cut -c1-60)). Thread dump written to the log."
      fi
    fi
    if (( $(date +%s) - last_sum >= SUMMARY )); then
      last_sum=$(date +%s)
      avg=$(grep "^Frame stats:" "$log" | tail -24 | awk '{s+=$3; n++} END{if(n) printf "%.1f", s/n}')
      echo "Summary: ~${avg:-?} FPS average over the last 2 min; $(grep -c 'memory pressure' "$log") VRAM warnings; $(grep -c '^Stall:' "$log") stalls >40 ms this session"
    fi
  fi
  sleep 3
done
