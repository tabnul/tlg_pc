#!/usr/bin/env bash
# Imports from the USB stick (label TLGDUMP, Itemzflow dump) the psarc files that GoldHEN's FTP truncated (>4 GiB),
# checking against what is already in dump/:
#   1. the bytes already downloaded (truncated prefix) must be identical on the stick;
#   2. the final size must match the PS4 LIST (dump/.filelist);
#   3. the other psarc files on the stick must be identical to the local ones (USB dump cross-check).
set -euo pipefail
dump=${DUMP_DIR:-$HOME/decomp/dump}
dev=/dev/disk/by-label/TLGDUMP
[[ -e $dev ]] || { echo "USB stick TLGDUMP not found" >&2; exit 1; }
mnt=$(findmnt -n -o TARGET "$dev" || true)
if [[ -z $mnt ]]; then udisksctl mount -b "$dev" >/dev/null; mnt=$(findmnt -n -o TARGET "$dev"); fi
src=$(find "$mnt" -maxdepth 3 -type f -name trico_boot.psarc -printf '%h\n' | head -1)
[[ -n $src ]] || { echo "No trico_boot.psarc in $mnt" >&2; exit 1; }
echo "Source: $src"
expected() { awk -v f="/$1" '$2 ~ f"$" {print $1}' "$dump/.filelist"; }
ok=1
for f in trico_001.psarc trico_002.psarc trico_003.psarc; do
    want=$(expected "$f"); have=$(stat -c %s "$src/$f")
    [[ $have == "$want" ]] || { echo "ERROR $f: stick has $have bytes, PS4 lists $want"; ok=0; continue; }
    part=$(stat -c %s "$dump/$f")
    if ! cmp -s -n "$part" "$src/$f" "$dump/$f"; then echo "ERROR $f: first $part bytes differ from the FTP download"; ok=0; continue; fi
    echo "$f: $part-byte prefix identical; copying the remaining $((want-part)) bytes..."
    tail -c +"$((part+1))" "$src/$f" >> "$dump/$f"
    [[ $(stat -c %s "$dump/$f") == "$want" ]] && echo "  OK $f complete ($want bytes)" || { echo "  ERROR final size of $f"; ok=0; }
done
for f in trico_boot.psarc trico_enu.psarc trico_esl.psarc trico_frc.psarc trico_ptb.psarc sce_discmap.plt; do
    [[ -f $src/$f ]] || { echo "warning: $f missing on the stick"; continue; }
    if cmp -s "$src/$f" "$dump/$f"; then echo "identical: $f"; else echo "ERROR: $f differs between stick and FTP"; ok=0; fi
done
(( ok )) && echo "IMPORT OK" || { echo "IMPORT FAILED"; exit 1; }
