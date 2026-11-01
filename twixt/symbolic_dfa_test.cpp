#include "symbolic_dfa.hpp"

#include <iostream>
#include <random>
#include <set>
#include <string>

using namespace twixt_dfa;

void require(bool test, const char* message) {
  if (!test) throw std::runtime_error(message);
}

// Exhaustively compare every operation to ordinary sets on a mixed
// binary/ternary schema.  Includes rejection, overlapping sets, and roots
// shared across collection; a wrong merge cannot hide behind a count.
// A large ID base makes every node ID exceed 32 bits.
void exhaustive(Id base) {
    Manager manager({3, 2, 3, 2, 3}, 100'000, {}, base);
    std::vector<Word> universe;
    for (unsigned n = 0; n < 108; ++n) {
      unsigned digits = n;
      Word word(5);
      for (unsigned i = 0; i < 5; ++i) {
        word[i] = digits % manager.alphabet()[i];
        digits /= manager.alphabet()[i];
      }
      universe.push_back(word);
    }
    std::mt19937 random(20261030);
    for (int trial = 0; trial < 32; ++trial) {
      std::set<Word> a, b;
      Id x = 0, y = 0;
      for (const auto& word : universe) {
        if (random() % 3 == 0) { a.insert(word); x = manager.unite(x, manager.singleton(word)); }
        if (random() % 4 == 0) { b.insert(word); y = manager.unite(y, manager.singleton(word)); }
      }
      require(manager.count(x) == a.size() && manager.count(y) == b.size(), "DFA count");
      Id both = manager.intersect(x, y), either = manager.unite(x, y);
      Id diff = manager.difference(x, y);
      // n-ary union of overlapping, nested, repeated, and empty operands.
      std::set<Word> c;
      Id z = 0;
      for (const auto& word : universe) {
        if (random() % 5 == 0) { c.insert(word); z = manager.unite(z, manager.singleton(word)); }
      }
      Id many = manager.unite_many({x, y, both, z, 0, x, diff});
      const int variable = trial % 5;
      const unsigned before = trial % manager.alphabet()[variable];
      const unsigned after = (before + 1) % manager.alphabet()[variable];
      auto allowed = manager.unrestricted();
      allowed[(variable + 1) % 5] = 1;
      Id changed = manager.filter(x, allowed, variable, before, after);
      std::set<Word> expected;
      for (auto word : a) {
        if (word[(variable + 1) % 5] == 0 && word[variable] == before) {
          word[variable] = static_cast<std::uint8_t>(after);
          expected.insert(word);
        }
      }
      // Pass an aliased root to catch accidental double-remapping during GC.
      manager.collect({&x, &y, &both, &either, &diff, &changed, &x, &many});
      for (const auto& word : universe) {
        const bool in_a = a.count(word), in_b = b.count(word);
        require(manager.contains(x, word) == in_a, "DFA collection/membership");
        require(manager.contains(both, word) == (in_a && in_b), "DFA intersection");
        require(manager.contains(either, word) == (in_a || in_b), "DFA union");
        require(manager.contains(diff, word) == (in_a && !in_b), "DFA difference");
        require(manager.contains(many, word) == (in_a || in_b || c.count(word) != 0), "DFA n-ary union");
        require(manager.contains(changed, word) == (expected.count(word) != 0), "DFA conditional rename");
      }
      require(manager.count(changed) == expected.size(), "DFA rename count");
      require(manager.difference(x, either) == 0, "DFA canonical rejection");

      const unsigned bit = trial % 2 == 0 ? 1 : 3;
      auto predicate = manager.unrestricted();
      predicate[0] = 2;  // tests dependencies before the assigned variable
      predicate[4] = 5;  // and dependencies after it
      Id image = manager.conditional_bit(x, predicate, bit, false);
      Id preimage = manager.conditional_bit(y, predicate, bit, true);
      std::set<Word> mapped;
      for (auto word : a) {
        if (word[bit] != 0) continue;
        word[bit] = word[0] == 1 && word[4] != 1 ? 1 : 0;
        mapped.insert(word);
      }
      for (auto word : universe) {
        require(manager.contains(image, word) == (mapped.count(word) != 0), "conditional bit image");
        const bool old_zero = word[bit] == 0;
        auto child = word;
        child[bit] = word[0] == 1 && word[4] != 1 ? 1 : 0;
        require(manager.contains(preimage, word) == (old_zero && b.count(child) != 0),
                "conditional bit preimage");
      }
    }
    for (Id id : {base}) require(manager.size() >= id, "ID base respected");
}

int main() {
  try {
    exhaustive(2);
    exhaustive((Id{1} << 32) + 7);
    exhaustive((Id{1} << 39) - 100'000);
    Manager large(std::vector<std::uint8_t>(64, 2));
    Id all = 1;
    for (int i = 63; i >= 0; --i) all = large.intern(i, {all, all, 0});
    bool overflow = false;
    try { (void)large.count(all); } catch (const std::overflow_error&) { overflow = true; }
    require(overflow, "position count overflow must be reported");
    std::cout << "symbolic DFA set algebra: all exhaustive checks passed\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "test failed: " << e.what() << '\n';
    return 1;
  }
}
