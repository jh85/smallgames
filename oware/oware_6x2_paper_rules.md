# Oware 6×2: Rules for Replicating Neumann and Gros

**Purpose:** Specify the Oware game to implement before constructing a strongly solved win/draw/loss (WDL) database.

**Board:** Two rows of six pits, initially four seeds per pit; 48 seeds total.

**Primary source:** Neumann and Gros, *AlphaZero Neural Scaling and Zipf's Law: a Tale of Board Games and Power Laws*, attached arXiv version 2412.11979v2, dated 13 October 2025. The Oware rules appear in Appendix B, page 14. [1]

**Source distinction:** Rules explicitly stated in the paper are identified below. Feeding and repetition details are supplied by OpenSpiel v1.5, the version recorded in the authors' analysis repository's dependency lockfile. This establishes an implementation reference, but does not prove the exact software revision used for every original training run. Any additional convention needed to make this specification executable is labeled explicitly. [2–5]

## 1. Rule summary

| Item | Specification | Evidence |
| --- | --- | --- |
| Players | Two players alternate individual moves | Paper |
| Board | Six pits per player, twelve pits total | Paper |
| Initial seeds | Four in every pit; 48 total | Paper |
| Initial captured scores | Zero for both players | OpenSpiel |
| Sowing | Counterclockwise, one seed at a time; skip the selected pit on every circuit | Paper |
| Capture trigger | The final sown seed lands in an opponent's pit and makes its count exactly 2 or 3 | Paper |
| Capture continuation | Move backwards through consecutive opponent pits containing exactly 2 or 3 seeds | Paper |
| Grand slam | Allow the sowing move, but cancel the entire capture if it would take every opponent seed | Paper and OpenSpiel |
| Feeding | If the opponent's row is empty, play a move that supplies it with seeds, if possible | OpenSpiel |
| No legal move | End the game and award the remaining seeds to the owners of their pits | Paper and OpenSpiel |
| Repetition | End at the first repeated complete board state and award remaining seeds by pit ownership | OpenSpiel |
| Ordinary win | Capture at least 25 seeds | Paper |
| Ordinary draw | Both players finish with 24 seeds | Paper |
| Length limit | Draw if the game remains unfinished after 1,000 individual moves | Paper; precise boundary convention in Section 8 |

**The grand-slam rule does not remove a move from the legal-move list merely because its ordinary capture would empty the opponent's row. The seeds are sown, then all captures for that move are cancelled. This applies even when another move is available.**

## 2. Board, players, and initial state

Use fixed player identities `P0` and `P1`. For implementation, use the following pit indexing, matching OpenSpiel:

| Row, viewed from P0's side | Leftmost | | | | | Rightmost |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| P1's row | 11 | 10 | 9 | 8 | 7 | 6 |
| P0's row | 0 | 1 | 2 | 3 | 4 | 5 |

- P0 owns pits `0..5`.
- P1 owns pits `6..11`.
- Sowing advances to `(i + 1) mod 12`.
- Capture proceeds backwards, decreasing the index within the opponent's row.
- The optional physical stores are score counters, not sowing pits.

The initial state is:

```text
seeds = [4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4]
captured = [0, 0]
side_to_move = P0
plies_played = 0
```

Choosing P0 as the first player is an indexing convention matching OpenSpiel. Captured seeds never return to the board. The conservation invariant is:

```text
sum(seeds) + captured[P0] + captured[P1] == 48
```

## 3. Legal moves and the feeding requirement

A move selects one nonempty pit owned by the player to move.

1. If the opponent has at least one seed in their row, every nonempty own pit is a legal choice.
2. If the opponent's row is empty, only choices whose sowing puts at least one seed into that row are legal.
3. If there is no such choice, the game ends under the no-legal-move rule in Section 7.

For the indexing above, let `u` be the last pit in the current player's row: `5` for P0 or `11` for P1. If the opponent is empty, selecting own pit `i` supplies the opponent exactly when:

```text
seeds[i] > u - i
```

There is no pass move. The rule does not permit a non-feeding move merely because every available sowing choice fails to feed; that situation ends the game.

## 4. Sowing

For a selected legal pit `i`:

1. Pick up all `seeds[i]` seeds and set that pit to zero.
2. Move counterclockwise around the twelve pits.
3. Deposit one seed in each successive pit.
4. Skip the originally selected pit whenever it is encountered, including on later circuits.
5. Stop after depositing the last seed, and record its destination as `last_pit`.

The selected pit remains empty after sowing. Do not sow into a score store. There is no relay sowing: landing in a nonempty pit does not start another sowing operation.

## 5. Capturing

Determine captures using the board **after all sowing is complete**.

1. If `last_pit` belongs to the moving player, capture nothing.
2. If it belongs to the opponent but contains neither 2 nor 3 seeds, capture nothing.
3. Otherwise, mark that pit for capture.
4. Inspect preceding pits in the reverse of the sowing direction. Continue marking pits while they belong to the opponent and each contains exactly 2 or 3 seeds.
5. Stop at the first pit with any other count, including zero, or at the boundary of the opponent's row.
6. Apply the grand-slam rule before removing any marked seeds.

If the capture is allowed, remove every seed from every marked pit and add their total to the moving player's captured score.

Captures never extend into the moving player's row and never jump over a non-capturable pit.

### Grand slam: cancel capture, preserve sowing

Let `candidate_capture` be the total seeds in the marked capture chain. Let `opponent_total` be the opponent's total seeds after sowing and before capture.

If a nonempty capture chain has:

```text
candidate_capture == opponent_total
```

then cancel the **entire** capture:

- Leave the board exactly as it was after sowing.
- Leave both captured scores unchanged.
- Complete the move and pass the turn to the opponent, subject to termination rules.

Do not undo sowing, ban the move, permit a partial capture, or award the wiped-out row to the moving player.

The paper states: “Capturing all of an opponent's seeds is forbidden; in that case no capture is made.” [1]

## 6. Repetition

**This section describes OpenSpiel v1.5 behavior. Appendix B does not explicitly specify repetition.** [3–5]

The repetition key includes:

```text
(seeds[0..11], captured[P0], captured[P1], side_to_move)
```

Identical pit contents with a different side to move are not the same state.

- Record the initial key before the first move.
- After sowing, any capture, and switching the side to move, check the new key.
- If the key has already occurred, end the game immediately and collect the remaining seeds by row ownership, as described in Section 7.
- Thus the second occurrence of a complete board state triggers termination; this is not a threefold-repetition rule.
- Otherwise, record the new key and continue.
- OpenSpiel clears the repetition set whenever a positive capture occurs, then records the resulting state. This is valid because captured seeds never re-enter play, so an earlier state with more seeds on the board cannot recur.
- A cancelled grand-slam capture takes zero seeds and therefore does not clear the repetition set.

**Repetition is not automatically a draw.** The final captured scores after collecting the remaining seeds determine the result.

## 7. Ordinary termination and scoring

The game ends when any of the following holds:

1. A player has captured at least 25 seeds: that player wins.
2. Both players have captured 24 seeds: the game is drawn.
3. The player to move has no legal move: collect the remaining seeds by ownership and compare final scores.
4. A complete state repeats: collect the remaining seeds by ownership and compare final scores.

To collect remaining seeds:

```text
captured[P0] += sum(seeds[0..5])
captured[P1] += sum(seeds[6..11])
set every pit to zero
```

The higher final score wins; `24–24` is a draw. Collection does not award every remaining seed to the player who just moved. Ownership determines the recipient.

OpenSpiel also sweeps remaining seeds when a score-based terminal state makes its legal-action list empty. This does not change the WDL result once someone has at least 25 seeds.

## 8. The paper's 1,000-turn draw rule

Appendix B states that a draw is also declared if the game lasts 1,000 turns. [1]

For this specification, interpret a turn as **one move by one player**, also called one ply. The limit is therefore 1,000 plies, not 1,000 pairs of moves. This interpretation matches OpenSpiel's action-history length convention. The counter starts at zero in the initial position and increases after every move; captures do not reset it.

**Boundary convention adopted here:** Complete the 1,000th move, apply ordinary termination and repetition scoring, and declare a draw only if the game is still unfinished. No 1,001st move is played. The paper does not explicitly resolve a win or repetition coinciding with the cutoff; this precedence is an implementation convention, not a separately verified statement by the authors.

A draw caused solely by the length limit has value zero for both players regardless of their current captured scores. Do not turn this cutoff into a score-based win by sweeping the remaining seeds.

### Source discrepancy to retain in replication notes

OpenSpiel v1.5 declares `kMaxGameLength = 1000`, but its Oware transition code enforces this with an action-history assertion rather than an automatic draw result. The older `AlphaZero-scaling-laws` match helper linked by the paper's repository catches Oware length exceptions and skips those matches. That behavior differs from Appendix B's stated draw rule. [3, 4, 6]

For the intended game in this document, implement the paper's stated draw rule explicitly. The publicly linked helper does not establish how every original experiment adjudicated the limit. Reproducing historical match statistics exactly would require resolving that discrepancy separately.

## 9. Order of processing a move

For a live state under this specification:

1. Generate legal moves, applying the feeding restriction.
2. If none exist, collect remaining seeds and finish.
3. Validate the selected move and sow its seeds.
4. Determine the backward capture chain.
5. Cancel the entire capture if it is a grand slam; otherwise execute the capture.
6. If a positive capture occurred, clear the recorded repetition keys.
7. Switch the side to move and increment `plies_played`.
8. Apply score-based termination, repetition termination, and the new player's no-legal-move termination. For repetition/no-move endings, collect seeds and determine WDL. Record a new repetition key if play continues.
9. If play is still unfinished and `plies_played == 1000`, return a draw.

This order specifies WDL behavior; implementations may combine checks when the outcome is equivalent. It does not change the paper's rules merely to simplify retrograde analysis.

## 10. Small examples for implementation review

These are constructed local examples, not claims that the positions are reachable from the initial state.

| Situation | Before the move | Expected result |
| --- | --- | --- |
| Backward capture | P0 selects pit 5 containing 2 seeds. Opponent pits 6, 7, 8 contain 1, 2, 4 respectively; other opponent pits are empty. | Sowing makes pits 6 and 7 contain 2 and 3. Capture both, gaining 5 seeds. Pit 8 retains its 4 seeds, so this is not a grand slam. |
| Grand slam despite an alternative | P0 pits 4 and 5 contain 1 seed each; P1 pit 6 contains 1; all other pits are empty. P0 chooses pit 5. | Pit 6 becomes 2, but capturing it would empty P1's row. Capture nothing. Pit 4 retains 1 and pit 6 retains 2. Choosing pit 4 was also legal; its availability does not invalidate pit 5. |
| Mandatory feeding | P1's row is empty. P0 pits 0 and 5 contain 1 seed each; other pits are empty. | Only pit 5 is legal. Selecting pit 0 would fail to feed P1. |
| Feeding impossible | P1's row is empty. P0 pit 0 contains 1 seed; every other pit is empty. | No legal move. End the game and add that remaining seed to P0's captured score. |
| Loop adjudication | After a move and side switch, the full repetition key matches a recorded key. | End immediately, collect seeds by row ownership, and compare scores. Do not automatically label the result a draw. |

## 11. Consequences for the ZDD WDL solver

ZDDs may represent states or result sets, but their use must preserve the game being solved.

The ordinary board description consists of the twelve pit counts, captured scores, and side to move. For **exact continuation under the repetition and cutoff rules above**, the state additionally needs:

- The number of plies already played, or the remaining allowance to 1,000.
- The relevant set of previously encountered repetition keys since the last positive capture.

These affect available future continuations and adjudication. A database indexed only by ordinary board descriptions is not automatically a complete strong solution of the history-dependent game.

In particular:

- Do not silently replace repeated-state scoring with “every cycle is a draw.”
- Do not omit the 1,000-ply limit while claiming exact equivalence to the paper's stated game.
- Do not reset history when evaluating a position extracted from a self-play trajectory unless the query is explicitly defined as a fresh-start evaluation.
- A fresh-start convention, such as an empty prior history and a new 1,000-ply allowance for every queried board, defines a particular evaluation task; it is not generally the same as continuing the original trajectory.
- An ordinary-board WDL table remains possible as a chosen target, but its equivalence to the full-state game must be proved, or its different rules/query convention must be documented.

The WDL perspective should be documented as the querying player's perspective or the side-to-move perspective. Numeric coding such as `WIN = +1`, `DRAW = 0`, `LOSS = -1` is a storage convention, not an additional game rule.

## 12. Sources

1. Neumann, O., and Gros, C. *AlphaZero Neural Scaling and Zipf's Law: a Tale of Board Games and Power Laws*. Attached version: arXiv:2412.11979v2, 13 October 2025. Section 4 identifies OpenSpiel; Appendix B, page 14, states the Oware rules and the 1,000-turn draw. [Paper, version 2](https://arxiv.org/abs/2412.11979v2).
2. Authors' analysis repository: [README](https://github.com/OrenNeumann/alphazero_zipfs_law) and [dependency lockfile](https://github.com/OrenNeumann/alphazero_zipfs_law/blob/main/poetry.lock), which records OpenSpiel 1.5.
3. OpenSpiel v1.5, [oware.h](https://github.com/google-deepmind/open_spiel/blob/v1.5/open_spiel/games/oware/oware.h): feeding, cancelled grand-slam captures, repetition scoring, and maximum game length.
4. OpenSpiel v1.5, [oware.cc](https://github.com/google-deepmind/open_spiel/blob/v1.5/open_spiel/games/oware/oware.cc): legal actions, sowing, captures, repetition tracking, and collection of remaining seeds.
5. OpenSpiel v1.5, [oware_board.h](https://github.com/google-deepmind/open_spiel/blob/v1.5/open_spiel/games/oware/oware_board.h) and [oware_board.cc](https://github.com/google-deepmind/open_spiel/blob/v1.5/open_spiel/games/oware/oware_board.cc): pit indexing, initial state, and repetition-key equality.
6. Older training/match repository linked from the analysis repository: [AZ_helper_lib.py](https://github.com/OrenNeumann/AlphaZero-scaling-laws/blob/main/AZ_helper_lib.py), Oware exception handling inside the match worker.

Source inspection and specification date: 21 September 2026. Repository links to `main` may change; OpenSpiel links above target the historical `v1.5` tag.
