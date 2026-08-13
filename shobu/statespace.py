from math import comb, log10

def per_board(squares, maxw, maxb):
    t = 0
    by = {}
    for w in range(maxw+1):
        for b in range(min(maxb, squares-w)+1):
            c = comb(squares, w)*comb(squares-w, b)
            t += c
            by[(w,b)] = c
    return t, by

print("=== Full Shobu: 4 boards, 4x4 each, 4W+4B per board ===")
t, by = per_board(16, 4, 4)
print(f"configs per board (w,b<=4): {t:.3e}   (4,4) only: {by[(4,4)]:.3e}")
print(f"4-board product upper bound: {t**4:.2e}  x2 turns")
print(f"weight at full stone count (4,4)^4: {by[(4,4)]**4:.2e}")

print("\n=== Variant A: 2 boards 3x3, 3W+3B per board ===")
t, by = per_board(9, 3, 3)
print(f"per board: {t:,}  product x2 turns: {2*t*t:.3e}   (3,3)^2: {by[(3,3)]**2:,}")

print("\n=== Variant B: 2 boards 3x3, 2W+2B per board ===")
t, by = per_board(9, 2, 2)
print(f"per board: {t:,}  product x2 turns: {2*t*t:.3e}")

print("\n=== Variant C: 2 boards 4x4, 2W+2B per board ===")
t, by = per_board(16, 2, 2)
print(f"per board: {t:,}  product x2 turns: {2*t*t:.3e}")

print("\n=== Variant D: 2 boards 4x4, 3W+3B per board ===")
t, by = per_board(16, 3, 3)
print(f"per board: {t:,}  product x2 turns: {2*t*t:.3e}")
