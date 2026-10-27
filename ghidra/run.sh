#!/bin/sh
# Imports each PS1 executable into a headless Ghidra project, analyses it as MIPS R3000,
# and exports function boundaries plus decompiled C into ghidra/out/.
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
ghidra_home=/opt/ghidra_12.1.3_PUBLIC
export JAVA_HOME=/opt/jdk-21.0.12.1+1/Contents/Home
export PATH="$JAVA_HOME/bin:$PATH"
headless="$ghidra_home/support/analyzeHeadless"
out="$root/ghidra/out"
project="$root/ghidra/project"
mkdir -p "$project" "$out"
for spec in scus:80013000:800ec4d4 frontend:80130000:801ca434 game:80013000:800bae1c rschool:80013000:800b2a80; do
  n=${spec%%:*}; rest=${spec#*:}; base=${rest%%:*}; entry=${rest#*:}
  if [ ! -f "$project/.imported-$n" ]; then
    "$headless" "$project" cmr -import "$root/game/bins/$n.bin" -loader BinaryLoader \
      -loader-baseAddr "0x$base" -processor "MIPS:LE:32:default" -max-cpu 8 \
      -scriptPath "$root/ghidra/scripts" -preScript SeedPsx.java "$entry" \
      -analysisTimeoutPerFile 3600 -log "$out/import-$n.log" >/dev/null
    touch "$project/.imported-$n"
  fi
  "$headless" "$project" cmr -process "$n.bin" -noanalysis -readOnly \
    -scriptPath "$root/ghidra/scripts" -postScript ExportPsx.java "$out" ${1:-} \
    -log "$out/export-$n.log" >/dev/null
  echo "done $n"
done
