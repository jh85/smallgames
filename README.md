# Small game solvers

This repository contains solvers and reproducible result-generation tools for small
combinatorial games.

## Included projects

- [Dobutsu Shogi](dobutsushogi/README.md) — a C++20 ZDD/retrograde strong solver for
  4×3 through 8×3 boards, with optional dense left/right-reflection reduction and
  reachability bits.
- [NOCCA × NOCCA](nocca/README.md) — a C++20 ZDD/retrograde strong solver for the 5×6
  game and its 3×3 variant, reproducing Yamamoto & Hoki (GPW 2022): all 147,969,899,280
  positions valued, initial position a first-player win decided at pass 41.
