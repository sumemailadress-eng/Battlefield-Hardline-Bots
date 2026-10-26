#!/bin/bash
SRV=$(grep -i "^Server=" "$(dirname "$0")/../Thugbot-Paths.ini" | cut -d= -f2- | tr -d '\r' | sed -E 's#^([A-Za-z]):#/\L\1#; s#\\#/#g')
# Vehicle summary for the current level: names seen, drives and how they ended, cars taken on foot; then per-team
# movement. Usage: bash tools/drivecheck.sh
L=$SRV/Thugbot_25200.log
printf 'bots' > $SRV/Thugbot_cmd.txt; sleep 3
grep -E "level: " $L | tail -1
S=$(grep -n "level: " $L | tail -1 | cut -d: -f1)
tail -n +$S $L | grep -E "vehicles: |drive: .* (gets out|takes)|drive: [0-9]+ bot|keeps its transform" \
  | sed -E 's/^[0-9:]+ //; s/ \(0000[0-9A-F]+\)//; s/vehicle [0-9A-F]+ /vehicle /; s/drive: [A-Za-z0-9 -]+ (gets out|takes)/drive: <bot> \1/' \
  | sort | uniq -c | sort -rn | head -16
bash "$(dirname "$0")/teamcheck.sh" 23:59 | head -2
