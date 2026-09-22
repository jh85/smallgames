"""Dump random OpenSpiel Oware transitions for cross-checking src/oware.hpp (see xcheck.cpp).

    openspiel_trace.py <games> <rng seed> [houses per player=6] [seeds per house=4]

Each line: the 2*houses seed counts, captured0, captured1, side to move, legal-action
mask, action, then the successor's seed counts, captured0, captured1 and a terminal flag.
"""
import random
import sys

import pyspiel


def parse(state):
    lines = [l for l in str(state).split("\n") if l.strip()]
    rows = [l for l in lines if l.split()[0].isdigit()]
    top = [int(x) for x in rows[0].split()]
    bottom = [int(x) for x in rows[1].split()]
    seeds = bottom + top[::-1]
    score = {int(l.split()[1]): int(l.split("=")[1].split()[0]) for l in lines if l.startswith("Player")}
    c0, c1 = score[0], score[1]
    return seeds, c0, c1


def main():
    games, seed = int(sys.argv[1]), int(sys.argv[2])
    houses = int(sys.argv[3]) if len(sys.argv) > 3 else 6
    per_house = int(sys.argv[4]) if len(sys.argv) > 4 else 4
    rng = random.Random(seed)
    game = pyspiel.load_game(f"oware(num_houses_per_player={houses},num_seeds_per_house={per_house})")
    for _ in range(games):
        s = game.new_initial_state()
        while not s.is_terminal():
            seeds, c0, c1 = parse(s)
            stm = s.current_player()
            legal = s.legal_actions()
            a = rng.choice(legal)
            s.apply_action(a)
            nseeds, n0, n1 = parse(s)
            mask = sum(1 << x for x in legal)
            print(*seeds, c0, c1, stm, mask, a, *nseeds, n0, n1, int(s.is_terminal()))


main()
