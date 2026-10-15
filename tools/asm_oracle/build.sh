#!/bin/sh
# Assemble the original code (from ../terep2isolado) into a flat image for run.py.
#   sh tools/asm_oracle/build.sh path/to/terep2isolado outdir
# Needs nasm; run.py needs python3-unicorn.  The only source change is
# DOS3Call (Win16) -> int 0x21, which run.py emulates.
set -e
SRC=$1; OUT=$2; HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$OUT"
sed 's/call far DOS3Call/int 0x21/' "$SRC/reasm/maincode.asm" > "$OUT/maincode_int.asm"
cp "$SRC/common/macros.asm" "$SRC/common/newvars_defs.asm" "$SRC/memdumps/data.bin" "$HERE/main.asm" "$HERE/run.py" "$HERE/dsmask.h" "$OUT/"
cd "$OUT" && nasm -f bin -o main.bin main.asm
echo "python3 $OUT/run.py trackdir frames [script] [ppmdir]"
