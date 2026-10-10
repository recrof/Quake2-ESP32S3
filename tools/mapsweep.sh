#!/bin/sh
# Loads maps one after the other through the serial logger (tools/logger.sh)
# and reports errors, fps and memory: tools/mapsweep.sh <logger workdir> map...
# (WAIT=<seconds> per map, default 50)
S=$1
shift
for m in "$@"; do
    : > "$S/console.log"
    echo "map $m" >> "$S/commands"
    sleep ${WAIT:-50}
    echo mem >> "$S/commands"
    sleep 2
    r=$(grep -av "models/" "$S/console.log" | grep -am1 "ERROR:\|Error" | cut -c10-120)
    f=$(grep -a "fps:" "$S/console.log" | tail -1 | cut -c10-)
    a=$(grep -a "arena " "$S/console.log" | tail -1 | cut -c25-110)
    echo "$m | ${r:-ok} | $f | $a"
    cp "$S/console.log" "$S/sweep-$m.log"
    grep -aq "Guru Meditation\|abort()\|Halted" "$S/console.log" && { echo "crashed, see $S/sweep-$m.log"; exit 1; }
done
