#!/bin/bash
SRV=$(grep -i "^Server=" "$(dirname "$0")/../Thugbot-Paths.ini" | cut -d= -f2- | tr -d '\r' | sed -E 's#^([A-Za-z]):#/\L\1#; s#\\#/#g')
# Bots that barely moved: two "bots" dumps $1 seconds apart (default 20); prints every living bot that moved under 3 m
# with its position, target, route progress and what it was doing, then a count per 8 m cell (where groups get stuck).
L=$SRV/Thugbot_25200.log
C=$SRV/Thugbot_cmd.txt
snap() {
    printf 'bots' > $C; sleep 3
    tail -c 400000 $L | grep -aE "^[0-9:]+ bot [0-9]+ .* team [12] at" | awk '{k=$3; line[k]=$0} END{for (k in line) print line[k]}'
}
A=$(mktemp); B=$(mktemp)
snap > $A; sleep ${1:-20}; snap > $B
awk 'NR==FNR { match($0, / at (-?[0-9]+) (-?[0-9]+) (-?[0-9]+) /, m); x[$3]=m[1]; z[$3]=m[3]; next }
     { match($0, / at (-?[0-9]+) (-?[0-9]+) (-?[0-9]+) /, m); if (!($3 in x)) next; dx=m[1]-x[$3]; dz=m[3]-z[$3];
       if (sqrt(dx*dx+dz*dz) < 3) { print; cell[int((m[1]+2000)/8)*8-2000 " " m[2] " " int((m[3]+2000)/8)*8-2000]++ } }
     END { for (c in cell) if (cell[c] > 1) print "GROUP " cell[c] " bots near " c }' $A $B
rm -f $A $B
