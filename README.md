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
- [Connect Four](connect4/README.md) — a C++20 ZDD/retrograde strong solver for 7×6 and
  smaller boards: 2,637,477,442,337 pseudo-legal non-terminal positions tabulated in ~9 h,
  validated record-for-record against an independent BDD solution.
- [N Men's Morris](nmm/README.md) — a C++20 two-ZDD strong solver extending Takeda & Hoki
  (IPSJ SIG-GI 2020) with placement-phase indexing and cyclic retrograde analysis. Twelve
  Men's Morris is a **first-player win**; a 16MM variant is implemented but stage-gated as
  infeasible on one machine.

Generated tables are kept out of git and published separately; each project's README
records how to regenerate them and the SHA-256 digests to verify a download against.
