#!/bin/bash
# summarize.sh: markdown table from results/<game>_n<NN>_<order>.txt (dfacomp output)
# columns: game, n, boards, table bytes, MV-DFA nodes, boards/node, MV-DFA sparse bytes,
#          threshold-set DFAs (count, nodes, sparse bytes), ratio sparse(all sets)/table
cd "$(dirname "$0")/results" || exit 1
echo "| Game | n | Boards | Table bytes | MV-DFA nodes | Boards/node | MV-DFA bytes (sparse) | Threshold sets | Their nodes | Their bytes (sparse) | DFA/table |"
echo "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |"
for f in $(ls *_n??_0,1,2,*.txt 2>/dev/null | sort); do
  game=${f%%_n*}; n=$(echo "$f" | sed -E 's/.*_n([0-9]+)_.*/\1/'); n=$((10#$n))
  grep -q '^done' "$f" || continue
  boards=$(sed -nE 's/^layer [0-9]+: ([0-9]+) boards.*/\1/p' "$f")
  tbytes=$(sed -nE 's/.*table ([0-9]+) bytes.*/\1/p' "$f")
  read nodes bpn sparse <<<"$(sed -nE 's/^  total ([0-9]+) nodes, [0-9]+ edges; boards\/node ([0-9.]+); dense [^,]+, sparse ([0-9.e+]+) B.*/\1 \2 \3/p' "$f")"
  read sets snodes ssparse <<<"$(sed -nE 's/^  all ([0-9]+) sets: ([0-9]+) nodes, [0-9]+ edges; dense [^,]+, sparse ([0-9.e+]+) B.*/\1 \2 \3/p' "$f")"
  ratio=$(awk -v a="$ssparse" -v b="$tbytes" 'BEGIN{printf "%.1fx", a/b}')
  printf "| %s | %s | %'d | %'d | %'d | %s | %s | %s | %'d | %s | %s |\n" "${game/_/\/}" "$n" "$boards" "$tbytes" "$nodes" "$bpn" "$sparse" "$sets" "$snodes" "$ssparse" "$ratio"
done
