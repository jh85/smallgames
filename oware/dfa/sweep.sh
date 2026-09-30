#!/bin/bash
# sweep.sh: run dfacomp over layers of the finished tables, several pit orders
R=/data1/smallgames/tmp/smallgames/oware
TH=${TH:-24}
run() { # bin dir seeds n order tag maxnodes
  out=results/$6_n$(printf %02d $4)_$5.txt
  [ -s "$out" ] && return
  echo "== $6 n=$4 order=$5" 
  $1 $2 $3 $4 $5 $TH ${7:-300000000} > $out 2>&1
  grep -E 'total|all .* sets|ABORTED|done' $out
}
# 7x2/28: orders
for n in 8 10 12 14 16 18 20; do
  run ./dfacomp7 $R/v28s 28 $n 0,1,2,3,4,5,6,7,8,9,10,11,12,13 7x2_28
  run ./dfacomp7 $R/v28s 28 $n 7,8,9,10,11,12,13,0,1,2,3,4,5,6 7x2_28
  run ./dfacomp7 $R/v28s 28 $n 13,12,11,10,9,8,7,6,5,4,3,2,1,0 7x2_28
  run ./dfacomp7 $R/v28s 28 $n 0,13,1,12,2,11,3,10,4,9,5,8,6,7 7x2_28
done
# 6x2/36
for n in 8 10 12 14 16 18 20; do
  run ./dfacomp6 $R/v36s 36 $n 0,1,2,3,4,5,6,7,8,9,10,11 6x2_36
  run ./dfacomp6 $R/v36s 36 $n 6,7,8,9,10,11,0,1,2,3,4,5 6x2_36
  run ./dfacomp6 $R/v36s 36 $n 11,10,9,8,7,6,5,4,3,2,1,0 6x2_36
  run ./dfacomp6 $R/v36s 36 $n 0,11,1,10,2,9,3,8,4,7,5,6 6x2_36
done
# 6x2/48
for n in 8 10 12 14 16 18 20; do
  run ./dfacomp6 $R/v48s 48 $n 0,1,2,3,4,5,6,7,8,9,10,11 6x2_48
  run ./dfacomp6 $R/v48s 48 $n 6,7,8,9,10,11,0,1,2,3,4,5 6x2_48
done
