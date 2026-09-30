#!/bin/bash
# pack_all.sh <lhdir> <seeds> <outdir> <row6|row7> [threads]: pack every layer of a
# finished table into gM-only .gmz files and verify each one exhaustively against
# the .lh files (gM equality and gN re-derived from the children).
LH=$1; SEEDS=$2; OUT=$3; ROW=$4; TH=${5:-24}
BIN=$(dirname "$0")/../build/gmz; [ "$ROW" = row7 ] && BIN=${BIN}7
mkdir -p "$OUT"
for f in "$LH"/oware_n*.lh; do
  n=$(basename "$f" .lh); n=${n#oware_n}; n=$((10#$n))
  [ -s "$OUT/oware_n$(printf %02d $n).gmz" ] && continue
  $BIN pack "$LH" "$SEEDS" "$n" "$OUT" 19 65536 0 "$TH" || exit 1
done
for f in "$LH"/oware_n*.lh; do
  n=$(basename "$f" .lh); n=${n#oware_n}; n=$((10#$n))
  $BIN verify "$LH" "$OUT" "$SEEDS" "$n" "$TH" || exit 1
done
echo "TOTAL .lh bytes: $(du -cb "$LH"/oware_n*.lh | tail -1 | cut -f1)  .gmz bytes: $(du -cb "$OUT"/oware_n*.gmz | tail -1 | cut -f1)"
