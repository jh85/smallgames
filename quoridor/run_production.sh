#!/bin/bash
# Production WDL solves: 6x6 w=0..4 and 7x7 w=0..3. Big tables saved to disk.
cd "$(dirname "$0")"
mkdir -p tables
set -x
./qwdl solve 6 6 0
./qwdl solve 6 6 1
./qwdl solve 6 6 2
./qwdl solve 6 6 3 --save tables/6x6_w3
time ./qwdl solve 6 6 4 --save tables/6x6_w4
./qwdl solve 7 7 0
./qwdl solve 7 7 1
./qwdl solve 7 7 2 --save tables/7x7_w2
time ./qwdl solve 7 7 3 --save tables/7x7_w3
echo ALL_DONE
