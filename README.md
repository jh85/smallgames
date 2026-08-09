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
- [Chinese Checkers](chinesecheckers/README.md) — a C++20 strong solver for two-player
  Chinese Checkers on m×m diamond boards, following Sturtevant (ACG 2019). The 6×6/6 game
  (2,313,100,389,600 positions) is solved in 19.8 h to a 144.7 GB WDL table; the initial
  position is a **first-player win**.
- [Breakthrough](breakthrough/README.md) — a C++20 ZDD strong solver for small Breakthrough
  boards, exploiting the game's acyclic advancement measure for a single backward sweep with
  no fixpoint. Five boards solved up to 5×6 (3,329,979,278,282 states): every 6-rank board
  is a first-player win, every shorter board a second-player win.
- [6×6 Othello](othello6x6/README.md) — an exact WDL/score oracle answering every legal
  position, by combining lookups in Takizawa's semi-strong tablebase (a third-party dataset,
  downloaded separately) with a built-in alpha–beta solver for whatever the table leaves
  undecided.
- [minigo](minigo/README.md) — Go on small boards under Tromp-Taylor rules, by parallel
  df-pn proof search rather than a backward sweep (Go's game graph is cyclic and superko
  makes a full table ill-posed). 5×5 proved to be **B+25** in 86 minutes; 7×7 is approached
  by solving selected book positions exactly.
- [Quoridor](quoridor/README.md) — a C++17/OpenMP strong solver using a ZDD over wall
  configurations plus layered retrograde analysis over the DAG of wall counts. 6×6 is a
  **first-player win** at every wall count solved (0–4); 7×7 is a **second-player win**
  through 3 walls per player.
- [TwixT](twixt/README.md) — an exact C++17 solver for auto-link computer TwixT on square
  and rectangular boards, with reachable-state census, layered strong retrograde analysis,
  portable WDL databases, and a database probe for policy evaluation.

Generated tables are kept out of git and published separately; each project's README
records how to regenerate them and the SHA-256 digests to verify a download against.
