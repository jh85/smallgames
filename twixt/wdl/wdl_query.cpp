// wdl_query.cpp - query a TwixT WDL package.
//
// Build:  g++ -O2 -std=c++17 -o wdl_query wdl_query.cpp
// Usage:  ./wdl_query PACKAGE_DIR [MOVE ...]
//         ./wdl_query twixt-6x6-wdl C3 D4 B2
//
// Plays the moves from the empty board (White first) and prints the
// game-theoretic outcome for the side to move: Win, Draw, or Loss.
//
// Rules: White joins top and bottom, Black joins left and right. A player may
// not place pegs on the opponent's border rows or columns; corners belong to
// nobody. A new peg is linked automatically to every own peg a knight's move
// away unless the link would cross an existing link. No pie rule.
//
// Data: layout.txt gives the board geometry and the order of the variables
// (turn, cells, links) along a DFA word. result-K.twxlay holds the positions
// after K moves as three minimal DFAs (roots: Win, Loss, Draw for the side to
// move). Nodes are stored grouped by level, deepest level first, so a single
// sequential pass decides membership of one word with one bit per node.

#include <cstdint>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

struct Layout {
  int width = 0, height = 0;
  std::vector<std::string> name;                  // cell -> "C3"
  std::vector<int> legal_white, legal_black;      // cell -> 0/1
  std::vector<int> edge_a, edge_b;                // link -> endpoint cells
  std::vector<std::vector<int>> crosses;          // link -> crossing links
  std::vector<char> kind;                         // level -> 't', 'c', 'l'
  std::vector<int> index;                         // level -> cell or link
};

static Layout read_layout(const std::string& path) {
  std::ifstream in(path);
  if (!in) throw std::runtime_error("cannot open " + path);
  Layout L;
  std::string line, word;
  while (std::getline(in, line)) {
    std::istringstream s(line);
    s >> word;
    if (word == "board") {
      s >> L.width >> L.height;
    } else if (word == "cell") {
      int c, w, b;
      std::string n;
      s >> c >> n >> w >> b;
      if (c >= static_cast<int>(L.name.size())) {
        L.name.resize(c + 1);
        L.legal_white.resize(c + 1);
        L.legal_black.resize(c + 1);
      }
      L.name[c] = n;
      L.legal_white[c] = w;
      L.legal_black[c] = b;
    } else if (word == "edge") {
      int e, a, b;
      s >> e >> a >> b;
      L.edge_a.resize(e + 1);
      L.edge_b.resize(e + 1);
      L.edge_a[e] = a;
      L.edge_b[e] = b;
    } else if (word == "cross") {
      int e, x;
      s >> e;
      if (e >= static_cast<int>(L.crosses.size())) L.crosses.resize(e + 1);
      while (s >> x) L.crosses[e].push_back(x);
    } else if (word == "level") {
      int i;
      std::string k;
      s >> i >> k;
      L.kind.resize(i + 1);
      L.index.resize(i + 1);
      L.kind[i] = k[0];
      L.index[i] = -1;
      if (k != "turn") s >> L.index[i];
    }
  }
  return L;
}

// Does `player` (1 White, 2 Black) connect its two borders through links?
// White joins the top and bottom rows, Black the left and right columns.
static bool connected(const Layout& L, const std::vector<int>& owner, const std::vector<int>& link, int player) {
  const int n = static_cast<int>(owner.size());
  auto start = [&](int c) { return player == 1 ? c / L.width == 0 : c % L.width == 0; };
  auto target = [&](int c) { return player == 1 ? c / L.width == L.height - 1 : c % L.width == L.width - 1; };
  std::vector<int> seen(n, 0), queue;
  for (int c = 0; c < n; ++c) if (owner[c] == player && start(c)) { seen[c] = 1; queue.push_back(c); }
  for (std::size_t h = 0; h < queue.size(); ++h) {
    const int c = queue[h];
    if (target(c)) return true;
    for (std::size_t e = 0; e < link.size(); ++e) {
      if (!link[e]) continue;
      const int other = L.edge_a[e] == c ? L.edge_b[e] : L.edge_b[e] == c ? L.edge_a[e] : -1;
      if (other >= 0 && !seen[other]) { seen[other] = 1; queue.push_back(other); }
    }
  }
  return false;
}

// Little-endian reads.
static std::uint64_t le(std::istream& in, int bytes) {
  unsigned char b[8] = {};
  if (!in.read(reinterpret_cast<char*>(b), bytes)) throw std::runtime_error("truncated layer file");
  std::uint64_t v = 0;
  for (int i = bytes - 1; i >= 0; --i) v = (v << 8) | b[i];
  return v;
}

int main(int argc, char** argv) {
  try {
    if (argc < 2) {
      std::cerr << "usage: " << argv[0] << " PACKAGE_DIR [MOVE ...]\n";
      return 2;
    }
    const std::string dir = argv[1];
    const Layout L = read_layout(dir + "/layout.txt");

    // Replay the moves.
    std::vector<int> owner(L.name.size(), 0);    // 0 empty, 1 White, 2 Black
    std::vector<int> link(L.edge_a.size(), 0);
    int turn = 0;                                // 0 White, 1 Black
    for (int m = 2; m < argc; ++m) {
      const std::string mv = argv[m];
      int cell = -1;
      for (std::size_t c = 0; c < L.name.size(); ++c) if (L.name[c] == mv) cell = static_cast<int>(c);
      if (cell < 0) throw std::runtime_error("unknown cell " + mv);
      if ((turn == 0 ? L.legal_white[cell] : L.legal_black[cell]) == 0) throw std::runtime_error(mv + " is not legal for the mover");
      if (owner[cell] != 0) throw std::runtime_error(mv + " is occupied");
      owner[cell] = turn + 1;
      for (std::size_t e = 0; e < L.edge_a.size(); ++e) {
        const int other = L.edge_a[e] == cell ? L.edge_b[e] : L.edge_b[e] == cell ? L.edge_a[e] : -1;
        if (other < 0 || owner[other] != turn + 1 || link[e]) continue;
        bool blocked = false;
        for (int x : L.crosses[e]) blocked = blocked || link[x];
        if (!blocked) link[e] = 1;
      }
      if (connected(L, owner, link, turn + 1) && m + 1 < argc) {
        throw std::runtime_error("the game ended with " + mv + "; the move list continues");
      }
      turn = 1 - turn;
    }
    const int ply = argc - 2;

    // The position as a word: one symbol per level.
    std::vector<int> word(L.kind.size());
    for (std::size_t i = 0; i < L.kind.size(); ++i) {
      word[i] = L.kind[i] == 't' ? turn : L.kind[i] == 'c' ? owner[L.index[i]] : link[L.index[i]];
    }

    // Read layer `ply`: header, roots, and the chunk directory at the end.
    const std::string path = dir + "/result-" + std::to_string(ply) + ".twxlay";
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path);
    char magic[8];
    in.read(magic, 8);
    if (std::string(magic, 8) != "TWXLAY01" || le(in, 4) != 2) throw std::runtime_error("not a version-2 layer file");
    for (int i = 0; i < 6; ++i) le(in, 4);       // width, height, order, length, ply, kind
    const std::uint64_t root_count = le(in, 4), id_bytes = le(in, 4), count = le(in, 8);
    std::vector<std::uint64_t> roots(root_count);
    for (auto& r : roots) r = le(in, 8);         // Win, Loss, Draw
    in.seekg(0, std::ios::end);
    const std::uint64_t size = static_cast<std::uint64_t>(in.tellg());
    in.seekg(static_cast<std::streamoff>(size - 16));
    const std::uint64_t directory = le(in, 8);
    in.seekg(static_cast<std::streamoff>(directory));
    struct Chunk { std::uint64_t offset, first, records; };
    struct Block { std::uint64_t level; std::vector<Chunk> chunks; };
    std::vector<Block> blocks(le(in, 4));
    for (auto& b : blocks) {
      b.level = le(in, 4);
      le(in, 8);                                 // node count
      b.chunks.resize(le(in, 8));
      for (auto& c : b.chunks) {
        c.offset = le(in, 8);
        c.first = le(in, 8);
        c.records = le(in, 8);
      }
    }

    // accept[node]: the node accepts the remaining suffix of `word`. Local ID
    // 0 is the empty language and 1 the accepting terminal. Children are
    // always on the next (deeper, already processed) level.
    std::vector<std::uint8_t> accept((count + 2 + 7) / 8, 0);
    auto get = [&](std::uint64_t id) { return (accept[id >> 3] >> (id & 7)) & 1; };
    accept[0] = 2;                               // id 1 accepts
    std::vector<char> buffer(1 << 20);
    in.rdbuf()->pubsetbuf(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    for (const Block& b : blocks) {
      const int symbol = word[b.level];
      for (const Chunk& c : b.chunks) {
        in.seekg(static_cast<std::streamoff>(c.offset));
        std::uint64_t self = c.first;
        for (std::uint64_t r = 0; r < c.records; ++r) {
          std::uint64_t delta = 0;
          for (int shift = 0;; shift += 7) {      // LEB128 local-ID delta
            const int byte = in.get();
            if (byte < 0) throw std::runtime_error("truncated layer file");
            delta |= static_cast<std::uint64_t>(byte & 0x7f) << shift;
            if ((byte & 0x80) == 0) break;
          }
          self += delta;
          const int mask = in.get();             // arcs present, symbols 0..2
          std::uint64_t child = 0;
          for (int s = 0; s < 3; ++s) {
            if ((mask & (1 << s)) == 0) continue;
            const std::uint64_t id = le(in, static_cast<int>(id_bytes));
            if (s == symbol) child = id;
          }
          if (child != 0 && get(child)) accept[self >> 3] |= static_cast<std::uint8_t>(1u << (self & 7));
        }
      }
    }

    const char* who = turn == 0 ? "White" : "Black";
    const char* names[3] = {"Win", "Loss", "Draw"};
    for (int i = 0; i < 3; ++i) {
      if (roots[i] >= 1 && get(roots[i])) {
        std::cout << "ply " << ply << ", " << who << " to move: " << names[i] << "\n";
        return 0;
      }
    }
    std::cout << "ply " << ply << ": position not reachable (illegal sequence or game already over)\n";
    return 1;
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 2;
  }
}
