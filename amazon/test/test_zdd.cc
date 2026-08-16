// amazons — ZDD manager and verdict database tests.
#include <cstdio>
#include <vector>

#include "board.h"
#include "zdd.h"

using namespace amazons;

#define CHECK(cond)                                       \
  do {                                                    \
    if (!(cond)) {                                        \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, \
                   __LINE__, #cond);                      \
      return 1;                                           \
    }                                                     \
  } while (0)

static int TestBasics() {
  ZddManager m;
  CHECK(m.Contains(ZddManager::kUnit, {}));
  CHECK(!m.Contains(ZddManager::kEmpty, {}));
  const ZddManager::NodeId a = m.Singleton({1, 3, 5});
  const ZddManager::NodeId b = m.Singleton({2, 4});
  CHECK(m.Contains(a, {1, 3, 5}));
  CHECK(!m.Contains(a, {1, 3}));
  CHECK(!m.Contains(a, {1, 3, 5, 7}));
  CHECK(!m.Contains(a, {}));
  const ZddManager::NodeId u = m.Union(a, b);
  CHECK(m.Contains(u, {1, 3, 5}));
  CHECK(m.Contains(u, {2, 4}));
  CHECK(!m.Contains(u, {1, 3}));
  CHECK(m.Count(u) == 2);
  // Union is idempotent; duplicate insert must not double-count.
  CHECK(m.Union(u, a) == u);
  CHECK(m.Count(m.Union(u, a)) == 2);
  // Empty set member.
  const ZddManager::NodeId w = m.Union(u, m.Singleton({}));
  CHECK(m.Contains(w, {}));
  CHECK(m.Count(w) == 3);
  return 0;
}

static int TestVerdictDb() {
  // Collect positions along a playout of 3x3 one-queen amazons.
  std::vector<Position> path;
  Position p = Position::OneQueen(3, 3, 0, 0, 2, 2);
  path.push_back(p.Canonical());
  std::vector<Move> moves;
  for (int i = 0; i < 3; i++) {
    p.GenerateMoves(&moves);
    if (moves.empty()) break;
    p.DoMove(moves[i % moves.size()]);
    path.push_back(p.Canonical());
  }
  CHECK(path.size() >= 2);

  VerdictDb db;
  for (size_t i = 0; i < path.size(); i++) {
    if (i % 2 == 0)
      db.InsertWin(path[i]);
    else
      db.InsertLoss(path[i]);
  }
  for (size_t i = 0; i < path.size(); i++)
    CHECK(db.Probe(path[i]) == (i % 2 == 0 ? 1 : -1));
  CHECK(db.num_wins() == (path.size() + 1) / 2);
  CHECK(db.num_losses() == path.size() / 2);

  // A position not on the path must probe 0.
  const Position off = Position::OneQueen(3, 3, 0, 0, 1, 2).Canonical();
  CHECK(db.Probe(off) == 0);

  // Re-inserting must be a no-op for the counts.
  db.InsertWin(path[0]);
  CHECK(db.num_wins() == (path.size() + 1) / 2);

  // Compression sanity: a batch of nearby positions must use far fewer
  // ZDD nodes than 2 variables per square per position.
  VerdictDb db2;
  Position q = Position::Standard(4, 4);
  std::vector<Move> ms;
  for (int i = 0; i < 200; i++) {
    q.GenerateMoves(&ms);
    if (ms.empty()) break;
    db2.InsertWin(q.Canonical());
    for (Move m : ms) {  // siblings share almost all squares
      Position c = q;
      c.DoMove(m);
      db2.InsertLoss(c.Canonical());
    }
    q.DoMove(ms[0]);
  }
  CHECK(db2.num_wins() > 0);
  CHECK(db2.zdd_nodes() < 20 * (db2.num_wins() + db2.num_losses()));
  return 0;
}

static int TestSaveLoad() {
  // Build a DB, save, reload, compare probes and counts.
  VerdictDb db;
  Position q = Position::Standard(4, 4);
  std::vector<Move> ms;
  for (int i = 0; i < 50; i++) {
    q.GenerateMoves(&ms);
    if (ms.empty()) break;
    db.InsertWin(q.Canonical());
    for (size_t k = 0; k < ms.size() && k < 5; k++) {
      Position c = q;
      c.DoMove(ms[k]);
      db.InsertLoss(c.Canonical());
    }
    q.DoMove(ms[0]);
  }
  const char* path = "/tmp/amazons_test_wdl.bin";
  CHECK(db.Save(path, 4, 4));

  VerdictDb db2;
  CHECK(db2.Load(path, 4, 4));
  CHECK(db2.num_wins() == db.num_wins());
  CHECK(db2.num_losses() == db.num_losses());
  CHECK(db2.zdd_nodes() == db.zdd_nodes());
  // Spot-check probes agree on a batch of positions.
  q = Position::Standard(4, 4);
  for (int i = 0; i < 30; i++) {
    q.GenerateMoves(&ms);
    if (ms.empty()) break;
    CHECK(db2.Probe(q.Canonical()) == db.Probe(q.Canonical()));
    for (Move m : ms) {
      Position c = q;
      c.DoMove(m);
      CHECK(db2.Probe(c.Canonical()) == db.Probe(c.Canonical()));
    }
    q.DoMove(ms.back());
  }
  // Wrong board size must be refused.
  VerdictDb db3;
  CHECK(!db3.Load(path, 5, 4));
  std::remove(path);
  return 0;
}

int main() {
  if (int r = TestBasics()) return r;
  if (int r = TestVerdictDb()) return r;
  if (int r = TestSaveLoad()) return r;
  std::printf("test_zdd OK\n");
  return 0;
}
