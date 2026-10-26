#!/bin/bash
SRV=$(grep -i "^Server=" "$(dirname "$0")/../Thugbot-Paths.ini" | cut -d= -f2- | tr -d '\r' | sed -E 's#^([A-Za-z]):#/\L\1#; s#\\#/#g')
# Do bots stuck in a spawn vehicle get out with the given command? Usage: bash tools/exittest.sh "press exit" "press exit"
#   (first argument turns it on, second turns it off). Lists stuck bots before and 6 s after.
L=$SRV/Thugbot_25200.log
C=$SRV/Thugbot_cmd.txt
snap() {
    printf 'bots' > $C; sleep 3
    S=$(grep -n "brain: level" $L | tail -1 | cut -d: -f1)
    tail -n +$S $L | grep -E "^[0-9:]+ bot [0-9]+ .* team" | sed -E 's/^[0-9:]+ bot ([0-9]+) .* team ([12]) at (-?[0-9]+) (-?[0-9]+) (-?[0-9]+) .*moving (-?[0-9.]+)$/\1 \2 \3 \5/'
}
A=$(mktemp); B=$(mktemp)
snap > $A
printf "$1" > $C; sleep 1.5; printf "$2" > $C
sleep 6
snap > $B
join <(sort $A) <(sort $B) | awk '{dx=$6-$3; dz=$7-$4; d=sqrt(dx*dx+dz*dz); n[$2]++; if(d<2) idle[$2]++} END{for(t in n) printf "after: team %s: %d bots, %d moved under 2 m in ~10 s\n", t, n[t], idle[t]}'
rm -f $A $B
