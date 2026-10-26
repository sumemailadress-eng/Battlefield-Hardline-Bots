#!/bin/bash
SRV=$(grep -i "^Server=" "$(dirname "$0")/../Thugbot-Paths.ini" | cut -d= -f2- | tr -d '\r' | sed -E 's#^([A-Za-z]):#/\L\1#; s#\\#/#g')
# Per-team movement check: two "bots" dumps 15 s apart; how far each team's living bots moved, and flag captures since
# the time given as $1 (HH:MM). Usage: bash tools/teamcheck.sh 21:03
L=$SRV/Thugbot_25200.log
C=$SRV/Thugbot_cmd.txt
snap() {
    printf 'bots' > $C; sleep 3
    S=$(grep -n "brain: level" $L | tail -1 | cut -d: -f1)
    tail -n +$S $L | grep -E "^[0-9:]+ bot [0-9]+ .* team" | sed -E 's/^[0-9:]+ bot ([0-9]+) .* team ([12]) at (-?[0-9]+) (-?[0-9]+) (-?[0-9]+) -> ([a-z]+) .*/\1 \2 \3 \5 \6/'
}
A=$(mktemp); B=$(mktemp)
snap > $A; sleep 15; snap > $B
join <(sort $A) <(sort $B) | awk '{dx=$7-$3; dz=$8-$4; d=sqrt(dx*dx+dz*dz); n[$2]++; s[$2]+=d; if(d<3) idle[$2]++} END{for(t in n) printf "team %s: %d bots alive, avg moved %.1f m in 15 s, %d moved under 3 m\n", t, n[t], s[t]/n[t], idle[t]}'
grep -E "flag [0-9] at .* -> team" $L | awk -v since="${1:-00:00}" '$1>since' | sed -E 's/.*-> (team [012]).*/captured by \1/' | sort | uniq -c
rm -f $A $B
