#!/bin/bash
# Strongly solve 7x2 Oware with 42 seeds (3 per pit) into 7x2_42/, auto mode.
# Restart wrapper: finished layers are reloaded from disk and the capture pass is
# idempotent, so only the current layer is lost when the process dies (e.g. an
# oomd kill); real bugs (exit 1/2/3/4) stop the loop.
# OW_CHECK_CAPTURE=1 enables a sampled brute-force check of each capture pass.
cd /data1/smallgames/tmp/smallgames/oware
mkdir -p 7x2_42
export OW_CHECK_CAPTURE=1
for i in $(seq 1 100); do
  echo "=== solve attempt $i $(date) ===" >> 7x2_42/solve.out
  numactl --interleave=all build/solve7 7x2_42 42 64 42 auto >> 7x2_42/solve.out 2>&1
  rc=$?
  echo "=== attempt $i exited rc=$rc $(date) ===" >> 7x2_42/solve.out
  case $rc in
    0) echo "solve complete" >> 7x2_42/solve.out; exit 0 ;;
    137|139|143|134) sleep 30 ;;  # killed: resume from finished layers
    *) echo "unrecoverable exit code $rc, stopping" >> 7x2_42/solve.out; exit $rc ;;
  esac
done
echo "gave up after 100 attempts" >> 7x2_42/solve.out
exit 1
