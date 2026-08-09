#!/usr/bin/env python3
"""Extract KataGo 7x7 book positions from the HTML tarball into a TSV.

Output columns (one row per book page = one position):
  board   49 chars, '.XO' row-major (row 1 = top, a-g left-right)
  next    'b' or 'w'
  bestwl  win-loss value of best move (KataGo's wl, +1 = next player wins-ish
          ... as stored in the page's moves[0])
  bestss  score mean of best move (ssM)
  visits  visits of best move
  nmoves  number of evaluated moves on the page

Usage: extract_book.py BOOK.tar.gz OUT.tsv
"""
import sys, tarfile, re, ast

def main():
    tar_path, out_path = sys.argv[1], sys.argv[2]
    rx_board = re.compile(r"const board = \[([0-9,]+)\]")
    rx_next = re.compile(r"const nextPla = (\d)")
    rx_moves = re.compile(r"const moves = (\[.*\])")
    n_pages = 0
    with tarfile.open(tar_path, "r:gz") as tf, open(out_path, "w") as out:
        out.write("board\tnext\tbestwl\tbestss\tvisits\tnmoves\n")
        for m in tf:
            if not m.name.endswith(".html"):
                continue
            data = tf.extractfile(m).read().decode("utf-8", "replace")
            bm = rx_board.search(data)
            nm = rx_next.search(data)
            if not bm or not nm:
                continue
            cells = [int(x) for x in bm.group(1).split(",") if x.strip()]
            board = "".join(".XO"[c] for c in cells[:49])
            nxt = "b" if nm.group(1) == "1" else "w"
            best_wl = best_ss = ""
            visits = nmoves = 0
            mm = rx_moves.search(data)
            if mm:
                txt = mm.group(1)
                txt = re.sub(r",\s*}", "}", re.sub(r",\s*\]", "]", txt))
                try:
                    moves = ast.literal_eval(txt)
                    nmoves = len(moves)
                    if moves:
                        best = max(moves, key=lambda d: d.get("v", 0))
                        best_wl = f"{best.get('wl', 0):.4f}"
                        best_ss = f"{best.get('ssM', 0):.2f}"
                        visits = best.get("v", 0)
                except (ValueError, SyntaxError):
                    pass
            out.write(f"{board}\t{nxt}\t{best_wl}\t{best_ss}\t{visits}\t{nmoves}\n")
            n_pages += 1
            if n_pages % 20000 == 0:
                print(f"{n_pages} pages", file=sys.stderr)
    print(f"done: {n_pages} positions -> {out_path}", file=sys.stderr)

if __name__ == "__main__":
    main()
