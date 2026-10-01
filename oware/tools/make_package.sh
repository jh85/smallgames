#!/bin/bash
# make_package.sh <gmzdir> <seeds> <row> <stats.txt> <outdir>
#
# Assembles a distributable Oware WDL package from a directory of verified .gmz
# layer files: tables/, checksums, the solver's stats.txt, the query programs
# (C++ probez + Python oware_probe.py) with their sources, and a README.
set -e
GMZ=$1; SEEDS=$2; ROW=$3; STATS=$4; OUT=$5
[ -d "$GMZ" ] && [ -n "$OUT" ] || { echo "usage: make_package.sh <gmzdir> <seeds> <row> <stats.txt> <outdir>"; exit 1; }
SRC=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$OUT/tables" "$OUT/src" "$OUT/python"
n=$(ls "$GMZ"/oware_n*.gmz | wc -l)
[ "$n" = "$SEEDS" ] || { echo "expected $SEEDS layer files in $GMZ, found $n"; exit 1; }
# hard-link the tables when the package lives on the same file system, else copy
for f in "$GMZ"/oware_n*.gmz; do
  t="$OUT/tables/$(basename "$f")"
  [ "$f" -ef "$t" ] && continue
  rm -f "$t"; ln "$f" "$t" 2>/dev/null || cp "$f" "$t"
done
[ -f "$STATS" ] && cp "$STATS" "$OUT/stats.txt"
cp "$SRC/src/oware.hpp" "$SRC/src/layer.hpp" "$SRC/src/gmz.hpp" "$SRC/src/gmz.cpp" "$SRC/src/probez.cpp" "$OUT/src/"
cp "$SRC/tools/oware_probe.py" "$OUT/python/"
cat > "$OUT/src/Makefile" <<EOF
# Builds the query program for this package (needs g++ >= 13 and libzstd-dev).
CXX ?= g++
CXXFLAGS ?= -O2 -std=c++20 -Wall
ROW = $ROW
all: probez gmz
probez: probez.cpp gmz.hpp oware.hpp
	\$(CXX) \$(CXXFLAGS) -DOWARE_ROW=\$(ROW) -o \$@ probez.cpp -lzstd
gmz: gmz.cpp gmz.hpp oware.hpp layer.hpp
	\$(CXX) \$(CXXFLAGS) -DOWARE_ROW=\$(ROW) -o \$@ gmz.cpp -lzstd
clean:
	rm -f probez gmz
EOF
(cd "$OUT" && sha256sum tables/oware_n*.gmz > SHA256SUMS)
PITS=$((2 * ROW)); PER=$((SEEDS / PITS)); WIN=$((SEEDS / 2 + 1)); HALF=$((SEEDS / 2))
BYTES=$(du -cb "$OUT"/tables/*.gmz | tail -1 | cut -f1)
GB=$(awk -v b="$BYTES" 'BEGIN{printf "%.1f", b/1e9}')
INIT_PITS=$(python3 -c "print(' '.join(['$PER']*$PITS))")
EXAMPLE_PITS=$(python3 -c "
import random; random.seed(3)
p=[$PER]*$PITS
# play a few random legal moves from the initial position with the package rules
import sys; sys.path.insert(0,'$SRC/tools')
from oware_probe import Rules
R=Rules($ROW,$SEEDS); b=tuple(p); cap=[0,0]; stm=0
for _ in range(6):
    m=R.legal(b)
    if not m: break
    b,c=R.play(b,random.choice(m)); cap[stm]+=c; stm^=1
# b is mover-normalised for stm; convert to absolute numbering
absb=[0]*$PITS
for j in range($PITS): absb[(j+$ROW*stm)%$PITS]=b[j]
print(' '.join(map(str,absb)), cap[0], cap[1], stm)")
cat > "$OUT/README.md" <<EOF
# Oware ${ROW}x2 with ${SEEDS} seeds: strong solution (WDL table)

Every position of ${ROW}x2 Oware with ${PER} seeds per pit (${SEEDS} seeds, ${WIN} captured
seeds win, ${HALF}-${HALF} is a draw) that can arise after a move, solved by retrograde
analysis. The rules are those of OpenSpiel's \`oware\` game (Abapa: counter-clockwise
sowing skipping the origin pit, captures of 2 or 3 backwards through the
opponent's row, grand slam allowed but captures nothing, mandatory feeding, no
legal move ends the game with each side collecting its own row). The solver, the
rule specification and the validation are in
https://github.com/jh85/smallgames (directory \`oware\`).

## What is in the package

* \`tables/oware_nNN.gmz\`: one file per number NN of seeds on the board
  (layer $((SEEDS - 1)) does not exist: a single seed can never be captured).
  ${GB} GB in total; check a download with \`sha256sum -c SHA256SUMS\`.
* \`stats.txt\`: for every layer and score split, the number of positions that are
  a win, a loss, a forced draw or undecided (see below).
* \`src/\`: C++ query program \`probez\` and the table tool \`gmz\` (\`make\` in
  \`src/\`, needs g++ 13+ and libzstd-dev).
* \`python/oware_probe.py\`: the same query in pure Python (needs
  \`pip install zstandard\`, or the \`zstd\` command).

## How to query a position

Pit numbering follows OpenSpiel: player 0 owns pits 0..$((ROW - 1)), player 1 owns
pits ${ROW}..$((PITS - 1)); a move sows from pit i into i+1, i+2, ... Give the ${PITS} pit
counts, the seeds captured by player 0 and player 1, and the side to move:

    cd src && make
    ./probez ../tables ${INIT_PITS} 0 0 0            # the initial position
    ./probez ../tables ${EXAMPLE_PITS}

    python3 python/oware_probe.py tables ${EXAMPLE_PITS}

Output: the value of the position for the side to move and of every legal move,
with the number of seeds each side can force.

## What the values mean

Oware's repetition rule makes the outcome of a cycle depend on the history, so the
table does not store a single WDL bit. It stores, for every board, the number of
the remaining seeds the side to move can force (gM) by play that ends on its own
(a capture to ${WIN}, or a no-move sweep) along a line that never repeats a
position; the number the other side can force (gN) is the minimum of gM over the
board's successors and is recomputed by the query programs. From these and the
captured scores every state is one of:

| Value | Meaning |
| --- | --- |
| WIN / LOSS | forced under the real rules, from a fresh repetition history |
| DRAW | both sides can force exactly ${HALF} seeds |
| DRAW* | neither side can force a win by self-terminating play; the real game would be decided by the repetition rule, which depends on the history |

Values are stored clamped to the range that can still decide a win, draw or loss
for some legal score split, so a printed bound like "can force >= 23" may mean
"23 or fewer" when no score split could turn that into a win. The 1,000-ply cap
of some rule sets is not modelled.

## File format (\`.gmz\`)

A 56-byte header (\`GzHeader\` in \`src/gmz.hpp\`: magic \`OWAREGZ1\`, layer n, clamp
base A, number of value levels V, seeds, boards per block, packing, zstd level,
field width, number of boards, number of blocks, offset of the block table),
then the blocks: each holds the clamped gM code of 65,536 consecutive boards, one
byte each, compressed with zstd, then a table of (blocks + 1) little-endian
64-bit file offsets. Boards are numbered by the perfect hash in
\`src/oware.hpp\` (\`Index::rank\`): a layered decision diagram over the pits whose
nodes count the boards below them. \`gmz verify\` checks a package against the
solver's original \`.lh\` tables; every layer of this package was verified that way.
EOF
echo "package $OUT: $(ls "$OUT/tables" | wc -l) tables, $GB GB"
