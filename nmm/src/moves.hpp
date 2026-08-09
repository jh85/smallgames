// moves.hpp — generic atomic move generation (placement / movement / flying, with
// mandatory single capture on mill formation) for the whole Morris family.
//
// A successor is the complete resulting state; a move plus its capture is one atomic
// transition. Rules implemented per the paper + prompt:
//  - placement while the mover has pieces in hand; mills formed by the placement give
//    exactly one capture (a doubled mill still gives one);
//  - captured piece must not be in a mill unless all opposing pieces are in mills;
//  - if a mill forms while the opponent has no piece on the board, the capture is skipped;
//  - movement to adjacent empty points; a player with exactly 3 pieces flies (if the game
//    has flying); mills formed by the move (i.e., complete mills through the destination)
//    give one capture;
//  - full-board placement (12/16MM) is a draw, detected by the solver via wb+bb == m.
#pragma once
#include "board.hpp"
#include <vector>

struct Succ {
  u32 w, b;          // resulting masks (not canonicalized)
  u8 wh, bh;         // resulting hands
  bool capture;      // a piece was captured by this move
};

// stm: 0 = white to move, 1 = black. Appends successors to out. Returns number of moves.
inline int genSuccessors(const Board& bd, u32 w, u32 b, int wh, int bh, int stm,
                         std::vector<Succ>& out) {
  const u32 all = (bd.m == 32) ? 0xFFFFFFFFu : ((1u << bd.m) - 1);
  u32 own = stm ? b : w, opp = stm ? w : b;
  int ownHand = stm ? bh : wh;
  u32 empty = all & ~(own | opp);
  int n0 = (int)out.size();

  // opponent pieces currently in complete mills (capture eligibility)
  auto oppMillPieces = [&]() {
    u32 mp = 0;
    for (u32 mk : bd.millMask)
      if ((opp & mk) == mk) mp |= mk;
    return mp;
  };
  auto emit = [&](u32 nown, u32 nopp, bool viaMill, int dWh, int dBh) {
    if (viaMill && (nopp != 0)) {
      u32 mp = 0;
      for (u32 mk : bd.millMask)
        if ((nopp & mk) == mk) mp |= mk;
      u32 cand = nopp & ~mp;
      if (!cand) cand = nopp;   // all in mills: any piece may be taken
      u32 t = cand;
      while (t) {
        u32 bit = t & (~t + 1);
        t ^= bit;
        u32 o2 = nopp ^ bit;
        out.push_back({stm ? o2 : nown, stm ? nown : o2, (u8)(wh + dWh), (u8)(bh + dBh), true});
      }
    } else {
      out.push_back({stm ? nopp : nown, stm ? nown : nopp, (u8)(wh + dWh), (u8)(bh + dBh), false});
    }
  };

  if (ownHand > 0) {   // placement
    u32 t = empty;
    while (t) {
      u32 bit = t & (~t + 1);
      t ^= bit;
      int p = __builtin_ctz(bit);
      u32 nown = own | bit;
      bool mill = false;
      u32 mills = bd.millsOfPoint[p];
      while (mills) {
        int mi = __builtin_ctz(mills);
        mills &= mills - 1;
        if ((nown & bd.millMask[mi]) == bd.millMask[mi]) { mill = true; break; }
      }
      emit(nown, opp, mill, stm ? 0 : -1, stm ? -1 : 0);
    }
  } else {             // movement / flying
    bool fly = bd.spec.flying && __builtin_popcount(own) == 3;
    u32 srcs = own;
    while (srcs) {
      u32 sbit = srcs & (~srcs + 1);
      srcs ^= sbit;
      int s = __builtin_ctz(sbit);
      u32 dests = (fly ? empty : (bd.adj[s] & empty));
      u32 d = dests;
      while (d) {
        u32 dbit = d & (~d + 1);
        d ^= dbit;
        int p = __builtin_ctz(dbit);
        u32 nown = (own ^ sbit) | dbit;
        bool mill = false;
        u32 mills = bd.millsOfPoint[p];
        while (mills) {
          int mi = __builtin_ctz(mills);
          mills &= mills - 1;
          if ((nown & bd.millMask[mi]) == bd.millMask[mi]) { mill = true; break; }
        }
        emit(nown, opp, mill, 0, 0);
      }
    }
  }
  (void)oppMillPieces;
  return (int)out.size() - n0;
}
