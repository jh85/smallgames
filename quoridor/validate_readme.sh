#!/bin/bash
# Reproduce known results from quoridor-solving README with the WDL solver.
cd "$(dirname "$0")"
pass=0; fail=0
check() { # W H w expected(Player1|Player2|Draw)
  out=$(./qwdl solve "$1" "$2" "$3" --quiet-layers 2>/dev/null | grep -o 'winner=[A-Za-z0-9]*')
  got=${out#winner=}
  if [ "$got" == "$4" ]; then
    echo "PASS ${1}x${2} w=$3 -> $got"; pass=$((pass+1))
  else
    echo "FAIL ${1}x${2} w=$3 -> got $got expected $4"; fail=$((fail+1))
  fi
}
check 2 3 0 Player2; check 2 3 1 Player1; check 2 3 3 Player1
check 3 3 0 Player2; check 3 3 3 Player2
check 4 3 2 Player2; check 4 3 3 Player1
check 5 3 2 Player2; check 5 3 3 Player1
check 6 3 3 Player2; check 6 3 4 Player1
check 7 3 5 Player2
check 8 3 2 Player2; check 8 3 3 Draw;    check 8 3 4 Player2
check 9 3 6 Player2; check 9 3 7 Player1
check 2 5 3 Player2
check 3 5 2 Player2; check 3 5 3 Player1; check 3 5 4 Player2
check 4 5 3 Player2; check 4 5 4 Player1
check 5 5 4 Player2; check 5 5 5 Player1
check 2 7 1 Player2; check 2 7 2 Player1
check 3 7 4 Player2; check 3 7 5 Player1
check 4 7 1 Player2; check 4 7 2 Player1; check 4 7 3 Player2; check 4 7 4 Player1
check 2 9 3 Player2
check 3 9 4 Player2; check 3 9 5 Player1
echo "== $pass passed, $fail failed =="
