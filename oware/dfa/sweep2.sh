#!/bin/bash
# sweep2.sh: default pit order only (orders differ by <15%), remaining layers
R=/data1/smallgames/tmp/smallgames/oware
TH=${TH:-32}
run() { out=results/$6_n$(printf %02d $4)_$5.txt; [ -s "$out" ] && return; echo "== $6 n=$4 order=$5"; $1 $2 $3 $4 $5 $TH ${7:-300000000} > $out 2>&1; grep -E 'total|all .* sets|ABORTED|done' $out; }
for n in 8 10 12 14 16 18 20; do run ./dfacomp6 $R/v36s 36 $n 0,1,2,3,4,5,6,7,8,9,10,11 6x2_36; done
for n in 8 12 16 20 24; do run ./dfacomp6 $R/v48s 48 $n 0,1,2,3,4,5,6,7,8,9,10,11 6x2_48; done
run ./dfacomp6 $R/v36s 36 24 0,1,2,3,4,5,6,7,8,9,10,11 6x2_36
