#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "symbolic_dfa.hpp"
#include "parallel_dfa.hpp"


namespace {

constexpr int kMaxDimension = 26;
constexpr int kMaxCells = 64;
// The maximum occurs near a square among rectangles with at most 64 cells.
// Keep a little headroom while reserving bit 191 for the player to move.
constexpr int kMaxEdges = 176;
constexpr int kLinkWords = (kMaxEdges + 63) / 64;

enum class Player : std::uint8_t { White = 0, Black = 1 };
enum class Outcome : std::int8_t { Loss = -1, Draw = 0, Win = 1 };

Player other(Player player) {
  return player == Player::White ? Player::Black : Player::White;
}

const char* player_name(Player player) {
  return player == Player::White ? "White" : "Black";
}

const char* outcome_name(Outcome outcome) {
  switch (outcome) {
    case Outcome::Win:
      return "win";
    case Outcome::Draw:
      return "draw";
    case Outcome::Loss:
      return "loss";
  }
  return "invalid";
}

Outcome negate(Outcome outcome) {
  return static_cast<Outcome>(-static_cast<int>(outcome));
}

struct State {
  std::uint64_t white = 0;
  std::uint64_t black = 0;
  std::array<std::uint64_t, kLinkWords> links{};
  Player turn = Player::White;
};

struct Edge {
  std::uint8_t a = 0;
  std::uint8_t b = 0;
};

struct Key {
  // Two words hold two-bit cell values; three words hold links.  The top bit
  // of the last link word holds the player to move (edge 191 is never used).
  std::array<std::uint64_t, 2 + kLinkWords> words{};

  bool operator==(const Key& rhs) const { return words == rhs.words; }
  bool operator<(const Key& rhs) const { return words < rhs.words; }
};

struct CanonicalContext {
  std::array<Key, 8> transformed{};
  int count = 1;
};

std::uint64_t mix64(std::uint64_t value) {
  value += 0x9e3779b97f4a7c15ULL;
  value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
  value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
  return value ^ (value >> 31);
}

struct KeyHash {
  std::size_t operator()(const Key& key) const noexcept {
    std::uint64_t hash = 0x243f6a8885a308d3ULL;
    for (std::uint64_t word : key.words) {
      hash ^= mix64(word + hash);
    }
    return static_cast<std::size_t>(hash);
  }
};

struct SearchStats {
  std::uint64_t nodes = 0;
  std::uint64_t tt_hits = 0;
  std::uint64_t draw_proofs = 0;
  std::uint64_t immediate_wins = 0;
  std::uint64_t tt_dropped = 0;
  std::uint64_t max_depth = 0;
};

struct Options {
  int width = 5;
  int height = 5;
  bool symmetry = true;
  bool draw_pruning = true;
  bool quiet = false;
  bool list_optimal = false;
  bool self_test = false;
  std::size_t tt_limit = 2'000'000;
  std::uint64_t progress_interval = 1'000'000;
  int pv_length = kMaxCells;
  std::string moves;
  std::optional<std::string> expected;
  bool estimate = false;
  int census_ply = -1;
  bool dfa_stats = false;
  std::size_t census_max_states = 1'000'000;
  double census_seconds = 10.0;
  bool strong = false;
  bool symbolic = false;
  bool symbolic_verify = false;
  int symbolic_ply = -1;
  std::size_t dfa_max_nodes = 5'000'000;
  unsigned workers = 1;
  std::size_t parallel_min_nodes = 20'000;
  std::uint64_t parallel_memory_gb = 512;
  std::string parallel_temp_directory = "tables";
  bool parallel_processes = false;
  std::string dfa_out;
  std::string probe_dfa;
  std::string dfa_checkpoint;
  std::string resume_dfa;
  std::string compare_dfa;
  std::string db_out;
  std::string probe_db;
};

class Twixt {
 public:
  Twixt(int width, int height)
      : width_(width), height_(height), cell_count_(width * height) {
    if (width < 3 || height < 3 || width > kMaxDimension ||
        height > kMaxDimension || cell_count_ > kMaxCells) {
      throw std::invalid_argument(
          "board dimensions must each be 3..26 and contain at most 64 cells");
    }
    for (int cell = 0; cell < cell_count_; ++cell) {
      for (Player player : {Player::White, Player::Black}) {
        const int p = static_cast<int>(player);
        if (legal_cell(cell, player)) legal_mask_[p] |= 1ULL << cell;
        if (on_start_border(cell, player)) start_mask_[p] |= 1ULL << cell;
        if (on_target_border(cell, player)) target_mask_[p] |= 1ULL << cell;
      }
    }
    for (auto& row : edge_for_) row.fill(-1);
    build_edges();
    build_crossings();
    build_transforms();
  }

  int width() const { return width_; }
  int height() const { return height_; }
  int cell_count() const { return cell_count_; }
  int symmetry_count() const { return width_ == height_ ? 8 : 4; }
  int cell_word_count() const { return (2 * cell_count_ + 63) / 64; }
  int link_word_count() const {
    return std::max(1, (edge_count() + 63) / 64);
  }
  int compact_key_word_count() const {
    return (2 * cell_count_ + edge_count() + 1 + 63) / 64;
  }
  int edge_count() const { return static_cast<int>(edges_.size()); }
  const Edge& edge(int edge_id) const { return edges_.at(edge_id); }
  const std::vector<int>& incident_edges(int cell) const { return incident_.at(cell); }
  bool edges_cross(int a, int b) const { return mask_has(crossings_.at(a), b); }

  bool valid_cell(int cell) const {
    const int x = cell % width_;
    const int y = cell / width_;
    return !((x == 0 || x == width_ - 1) &&
             (y == 0 || y == height_ - 1));
  }

  bool legal_cell(int cell, Player player) const {
    if (cell < 0 || cell >= cell_count_ || !valid_cell(cell)) return false;
    const int x = cell % width_;
    const int y = cell / width_;
    if (player == Player::White) return x != 0 && x != width_ - 1;
    return y != 0 && y != height_ - 1;
  }

  bool occupied(const State& state, int cell) const {
    return ((state.white | state.black) >> cell) & 1ULL;
  }

  std::uint64_t pegs(const State& state, Player player) const {
    return player == Player::White ? state.white : state.black;
  }

  int parse_cell(const std::string& token) const {
    std::string compact;
    for (char ch : token) {
      if (!std::isspace(static_cast<unsigned char>(ch))) compact.push_back(ch);
    }
    if (compact.size() < 2) throw std::invalid_argument("bad coordinate: " + token);
    const char column = static_cast<char>(std::toupper(static_cast<unsigned char>(compact[0])));
    if (column < 'A' || column >= 'A' + width_) {
      throw std::invalid_argument("column outside board: " + token);
    }
    int row = 0;
    for (std::size_t i = 1; i < compact.size(); ++i) {
      if (!std::isdigit(static_cast<unsigned char>(compact[i]))) {
        throw std::invalid_argument("bad coordinate: " + token);
      }
      row = row * 10 + (compact[i] - '0');
    }
    if (row < 1 || row > height_) {
      throw std::invalid_argument("row outside board: " + token);
    }
    return (row - 1) * width_ + (column - 'A');
  }

  std::string coordinate(int cell) const {
    std::string result(1, static_cast<char>('A' + cell % width_));
    result += std::to_string(cell / width_ + 1);
    return result;
  }

  std::vector<int> legal_moves(const State& state) const {
    std::vector<int> result;
    result.reserve(cell_count_);
    std::uint64_t moves = legal_mask_[static_cast<int>(state.turn)] &
                          ~(state.white | state.black);
    while (moves != 0) {
      const int cell = __builtin_ctzll(moves);
      result.push_back(cell);
      moves &= moves - 1;
    }
    return result;
  }

  // Places one peg and all currently legal links incident to it.  New links
  // share the new endpoint, so they cannot properly cross one another.
  State play(const State& state, int cell, int* links_added = nullptr) const {
    if (!legal_cell(cell, state.turn)) {
      throw std::invalid_argument(coordinate(cell) + " is forbidden for " + player_name(state.turn));
    }
    if (occupied(state, cell)) throw std::invalid_argument(coordinate(cell) + " is occupied");

    State next = state;
    const Player mover = state.turn;
    if (mover == Player::White) {
      next.white |= 1ULL << cell;
    } else {
      next.black |= 1ULL << cell;
    }

    int added = 0;
    const std::uint64_t friendly = pegs(state, mover);
    for (int edge_id : incident_[cell]) {
      const Edge& edge = edges_[edge_id];
      const int neighbor = edge.a == cell ? edge.b : edge.a;
      if (((friendly >> neighbor) & 1ULL) == 0) continue;
      if (!link_is_set(next, edge_id) && !crosses_existing(next, edge_id)) {
        set_link(next, edge_id);
        ++added;
      }
    }
    next.turn = other(state.turn);
    if (links_added != nullptr) *links_added = added;
    return next;
  }

  bool has_won(const State& state, Player player) const {
    const std::uint64_t own = pegs(state, player);
    std::uint64_t visited = own & start_mask_[static_cast<int>(player)];
    std::array<std::uint8_t, kMaxCells> queue{};
    int head = 0, tail = 0;
    for (std::uint64_t starts = visited; starts != 0; starts &= starts - 1) {
      queue[tail++] = static_cast<std::uint8_t>(__builtin_ctzll(starts));
    }
    while (head != tail) {
      const int cell = queue[head++];
      if (((target_mask_[static_cast<int>(player)] >> cell) & 1ULL) != 0) {
        return true;
      }
      for (int edge_id : incident_[cell]) {
        if (!link_is_set(state, edge_id)) continue;
        const Edge& edge = edges_[edge_id];
        const int neighbor = edge.a == cell ? edge.b : edge.a;
        if (((own >> neighbor) & 1ULL) == 0 || ((visited >> neighbor) & 1ULL) != 0) continue;
        visited |= 1ULL << neighbor;
        queue[tail++] = static_cast<std::uint8_t>(neighbor);
      }
    }
    return false;
  }

  // Sound over-approximation: empty usable holes are treated as future pegs,
  // and mutually crossing future links are all allowed.  Failure to find a
  // path therefore proves that this player can never connect.
  bool can_ever_win(const State& state, Player player) const {
    const std::uint64_t opponent = pegs(state, other(player));
    const int p = static_cast<int>(player);
    const std::uint64_t usable = legal_mask_[p] & ~opponent;
    std::uint64_t visited = usable & start_mask_[p];
    std::array<std::uint8_t, kMaxCells> queue{};
    int head = 0, tail = 0;
    for (std::uint64_t starts = visited; starts != 0; starts &= starts - 1) {
      queue[tail++] = static_cast<std::uint8_t>(__builtin_ctzll(starts));
    }
    while (head != tail) {
      const int cell = queue[head++];
      if (((target_mask_[p] >> cell) & 1ULL) != 0) return true;
      for (int edge_id : incident_[cell]) {
        const Edge& edge = edges_[edge_id];
        const int neighbor = edge.a == cell ? edge.b : edge.a;
        if (((usable >> neighbor) & 1ULL) == 0 || ((visited >> neighbor) & 1ULL) != 0) continue;
        if (!link_is_set(state, edge_id) && crosses_existing(state, edge_id)) continue;
        visited |= 1ULL << neighbor;
        queue[tail++] = static_cast<std::uint8_t>(neighbor);
      }
    }
    return false;
  }

  Key canonical_key(const State& state, bool use_symmetry) const {
    Key best = packed_key(state, 0);
    if (!use_symmetry) return best;
    for (int transform = 1; transform < symmetry_count(); ++transform) {
      Key candidate = packed_key(state, transform);
      if (candidate < best) best = candidate;
    }
    return best;
  }

  // Prepare all transformed versions of a parent once.  Each child differs
  // only by one peg, its incident new links, and the side to move.
  CanonicalContext prepare_canonical(const State& state,
                                     bool use_symmetry) const {
    CanonicalContext context;
    context.count = use_symmetry ? symmetry_count() : 1;
    for (int transform = 0; transform < context.count; ++transform) {
      context.transformed[transform] = packed_key(state, transform);
    }
    return context;
  }

  Key canonical_child_key(const CanonicalContext& context,
                          const State& parent, const State& child,
                          int move) const {
    Key best;
    bool have_best = false;
    for (int transform = 0; transform < context.count; ++transform) {
      Key candidate = context.transformed[transform];
      const bool swap_players = transform >= 4;
      std::uint64_t value = parent.turn == Player::White ? 1ULL : 2ULL;
      if (swap_players) value = value == 1 ? 2 : 1;
      const int mapped_cell = cell_transform_[transform][move];
      const int cell_bit = 2 * mapped_cell;
      candidate.words[cell_bit / 64] |= value << (cell_bit % 64);

      for (int word = 0; word < link_word_count(); ++word) {
        std::uint64_t added = child.links[word] & ~parent.links[word];
        while (added != 0) {
          const int bit = __builtin_ctzll(added);
          const int edge = 64 * word + bit;
          const int mapped_edge = edge_transform_[transform][edge];
          candidate.words[2 + mapped_edge / 64] |=
              1ULL << (mapped_edge % 64);
          added &= added - 1;
        }
      }
      candidate.words.back() ^= 1ULL << 63;
      if (!have_best || candidate < best) {
        best = candidate;
        have_best = true;
      }
    }
    return best;
  }

  State unpack_key(const Key& key) const {
    State state;
    for (int cell = 0; cell < cell_count_; ++cell) {
      const int bit = 2 * cell;
      const std::uint64_t value = (key.words[bit / 64] >> (bit % 64)) & 3ULL;
      if (value == 1) state.white |= 1ULL << cell;
      if (value == 2) state.black |= 1ULL << cell;
    }
    for (int word = 0; word < kLinkWords; ++word) {
      state.links[word] = key.words[2 + word];
    }
    state.turn = ((state.links.back() >> 63) & 1ULL) != 0
                     ? Player::Black
                     : Player::White;
    state.links.back() &= ~(1ULL << 63);
    return state;
  }

  std::array<std::uint64_t, 2 + kLinkWords> compact_key(
      const Key& key) const {
    std::array<std::uint64_t, 2 + kLinkWords> result{};
    auto copy_bit = [&](int output_bit, bool value) {
      if (value) result[output_bit / 64] |= 1ULL << (output_bit % 64);
    };
    for (int bit = 0; bit < 2 * cell_count_; ++bit) {
      copy_bit(bit, ((key.words[bit / 64] >> (bit % 64)) & 1ULL) != 0);
    }
    const int link_offset = 2 * cell_count_;
    for (int edge = 0; edge < edge_count(); ++edge) {
      copy_bit(link_offset + edge,
               ((key.words[2 + edge / 64] >> (edge % 64)) & 1ULL) != 0);
    }
    copy_bit(link_offset + edge_count(),
             ((key.words.back() >> 63) & 1ULL) != 0);
    return result;
  }

  Key expand_compact_key(
      const std::array<std::uint64_t, 2 + kLinkWords>& compact) const {
    Key key;
    auto compact_bit = [&](int bit) {
      return ((compact[bit / 64] >> (bit % 64)) & 1ULL) != 0;
    };
    for (int bit = 0; bit < 2 * cell_count_; ++bit) {
      if (compact_bit(bit)) key.words[bit / 64] |= 1ULL << (bit % 64);
    }
    const int link_offset = 2 * cell_count_;
    for (int edge = 0; edge < edge_count(); ++edge) {
      if (compact_bit(link_offset + edge)) {
        key.words[2 + edge / 64] |= 1ULL << (edge % 64);
      }
    }
    if (compact_bit(link_offset + edge_count())) key.words.back() |= 1ULL << 63;
    return key;
  }

  void print_board(const State& state, std::ostream& out) const {
    out << "   ";
    for (int x = 0; x < width_; ++x) out << ' ' << static_cast<char>('A' + x);
    out << '\n';
    for (int y = 0; y < height_; ++y) {
      out << std::setw(2) << y + 1 << ' ';
      for (int x = 0; x < width_; ++x) {
        const int cell = y * width_ + x;
        char marker = '.';
        if (!valid_cell(cell)) marker = ' ';
        if ((state.white >> cell) & 1ULL) marker = 'W';
        if ((state.black >> cell) & 1ULL) marker = 'B';
        out << ' ' << marker;
      }
      out << '\n';
    }
  }

  void self_test() const {
    if (width_ != 5 || height_ != 5) {
      throw std::logic_error("internal tests require a 5x5 board");
    }

    const int top_left = 0;
    if (valid_cell(top_left)) throw std::logic_error("corner was accepted");
    if (legal_cell(parse_cell("A3"), Player::White)) throw std::logic_error("White border rule failed");
    if (legal_cell(parse_cell("C1"), Player::Black)) throw std::logic_error("Black border rule failed");

    State linked;
    linked.turn = Player::White;
    linked = play(linked, parse_cell("B1"));
    linked.turn = Player::White;
    linked = play(linked, parse_cell("C3"));
    const int first_edge = edge_for_[parse_cell("B1")][parse_cell("C3")];
    if (first_edge < 0 || !link_is_set(linked, first_edge)) throw std::logic_error("auto-link failed");

    linked.turn = Player::White;
    linked = play(linked, parse_cell("B5"));
    if (!has_won(linked, Player::White)) throw std::logic_error("win detection failed");

    bool checked_crossing = false;
    for (int a = 0; a < edge_count() && !checked_crossing; ++a) {
      for (int b = a + 1; b < edge_count() && !checked_crossing; ++b) {
        if (!mask_has(crossings_[a], b)) continue;
        const Edge e1 = edges_[a];
        const Edge e2 = edges_[b];
        State blocked;
        blocked.black |= (1ULL << e1.a) | (1ULL << e1.b);
        set_link(blocked, a);
        blocked.white |= 1ULL << e2.a;
        blocked.turn = Player::White;
        if (!legal_cell(e2.b, Player::White) || occupied(blocked, e2.b)) continue;
        blocked = play(blocked, e2.b);
        if (link_is_set(blocked, b)) throw std::logic_error("crossing link was added");
        checked_crossing = true;
      }
    }
    if (!checked_crossing) throw std::logic_error("could not construct crossing test");

    State empty;
    const Key canonical = canonical_key(empty, true);
    for (int transform = 0; transform < symmetry_count(); ++transform) {
      if (packed_key(empty, transform) < canonical) {
        throw std::logic_error("empty-board canonicalization failed");
      }
    }

    State parent;
    parent = play(parent, parse_cell("B1"));
    parent = play(parent, parse_cell("A2"));
    const CanonicalContext context = prepare_canonical(parent, true);
    for (int move : legal_moves(parent)) {
      const State child = play(parent, move);
      if (!(canonical_child_key(context, parent, child, move) ==
            canonical_key(child, true))) {
        throw std::logic_error("incremental canonicalization failed");
      }
    }
  }

 private:
  int width_;
  int height_;
  int cell_count_;
  std::vector<Edge> edges_;
  std::array<std::vector<int>, kMaxCells> incident_{};
  std::array<std::array<int, kMaxCells>, kMaxCells> edge_for_{};
  std::array<std::array<std::uint64_t, kLinkWords>, kMaxEdges> crossings_{};
  std::array<std::array<std::uint8_t, kMaxCells>, 8> cell_transform_{};
  std::array<std::array<std::uint8_t, kMaxEdges>, 8> edge_transform_{};
  std::array<std::uint64_t, 2> legal_mask_{};
  std::array<std::uint64_t, 2> start_mask_{};
  std::array<std::uint64_t, 2> target_mask_{};

  static bool mask_has(const std::array<std::uint64_t, kLinkWords>& mask, int bit) {
    return ((mask[bit / 64] >> (bit % 64)) & 1ULL) != 0;
  }

  bool link_is_set(const State& state, int edge_id) const {
    return ((state.links[edge_id / 64] >> (edge_id % 64)) & 1ULL) != 0;
  }

  void set_link(State& state, int edge_id) const {
    state.links[edge_id / 64] |= 1ULL << (edge_id % 64);
  }

  bool crosses_existing(const State& state, int edge_id) const {
    for (int word = 0; word < link_word_count(); ++word) {
      if ((state.links[word] & crossings_[edge_id][word]) != 0) return true;
    }
    return false;
  }

  bool on_start_border(int cell, Player player) const {
    return player == Player::White ? cell / width_ == 0
                                   : cell % width_ == 0;
  }

  bool on_target_border(int cell, Player player) const {
    return player == Player::White ? cell / width_ == height_ - 1
                                   : cell % width_ == width_ - 1;
  }

  static long long orient(int ax, int ay, int bx, int by, int cx, int cy) {
    return static_cast<long long>(bx - ax) * (cy - ay) -
           static_cast<long long>(by - ay) * (cx - ax);
  }

  bool proper_cross(const Edge& lhs, const Edge& rhs) const {
    const int ax = lhs.a % width_, ay = lhs.a / width_;
    const int bx = lhs.b % width_, by = lhs.b / width_;
    const int cx = rhs.a % width_, cy = rhs.a / width_;
    const int dx = rhs.b % width_, dy = rhs.b / width_;
    const long long o1 = orient(ax, ay, bx, by, cx, cy);
    const long long o2 = orient(ax, ay, bx, by, dx, dy);
    const long long o3 = orient(cx, cy, dx, dy, ax, ay);
    const long long o4 = orient(cx, cy, dx, dy, bx, by);
    return ((o1 < 0 && o2 > 0) || (o1 > 0 && o2 < 0)) &&
           ((o3 < 0 && o4 > 0) || (o3 > 0 && o4 < 0));
  }

  void build_edges() {
    constexpr std::array<std::pair<int, int>, 8> directions{{
        {1, 2}, {2, 1}, {-1, 2}, {-2, 1}, {1, -2}, {2, -1}, {-1, -2}, {-2, -1}}};
    for (int a = 0; a < cell_count_; ++a) {
      if (!valid_cell(a)) continue;
      const int ax = a % width_, ay = a / width_;
      for (const auto& [dx, dy] : directions) {
        const int bx = ax + dx, by = ay + dy;
        if (bx < 0 || bx >= width_ || by < 0 || by >= height_) continue;
        const int b = by * width_ + bx;
        if (!valid_cell(b) || a >= b) continue;
        const int edge_id = static_cast<int>(edges_.size());
        if (edge_id >= kMaxEdges) throw std::logic_error("edge storage overflow");
        edges_.push_back(Edge{static_cast<std::uint8_t>(a), static_cast<std::uint8_t>(b)});
        incident_[a].push_back(edge_id);
        incident_[b].push_back(edge_id);
        edge_for_[a][b] = edge_for_[b][a] = edge_id;
      }
    }
  }

  void build_crossings() {
    for (int a = 0; a < edge_count(); ++a) {
      for (int b = a + 1; b < edge_count(); ++b) {
        if (!proper_cross(edges_[a], edges_[b])) continue;
        crossings_[a][b / 64] |= 1ULL << (b % 64);
        crossings_[b][a / 64] |= 1ULL << (a % 64);
      }
    }
  }

  std::pair<int, int> transformed_xy(int transform, int x, int y) const {
    switch (transform) {
      case 0:
        return {x, y};
      case 1:
        return {width_ - 1 - x, y};
      case 2:
        return {x, height_ - 1 - y};
      case 3:
        return {width_ - 1 - x, height_ - 1 - y};
      case 4:
        return {y, x};
      case 5:
        return {width_ - 1 - y, x};
      case 6:
        return {y, height_ - 1 - x};
      case 7:
        return {width_ - 1 - y, height_ - 1 - x};
      default:
        throw std::logic_error("bad transform");
    }
  }

  void build_transforms() {
    for (int transform = 0; transform < symmetry_count(); ++transform) {
      for (int cell = 0; cell < cell_count_; ++cell) {
        const auto [tx, ty] =
            transformed_xy(transform, cell % width_, cell / width_);
        cell_transform_[transform][cell] =
            static_cast<std::uint8_t>(ty * width_ + tx);
      }
      for (int edge_id = 0; edge_id < edge_count(); ++edge_id) {
        const int a = cell_transform_[transform][edges_[edge_id].a];
        const int b = cell_transform_[transform][edges_[edge_id].b];
        const int mapped = edge_for_[a][b];
        if (mapped < 0) throw std::logic_error("edge transform failed");
        edge_transform_[transform][edge_id] = static_cast<std::uint8_t>(mapped);
      }
    }
  }

  Key packed_key(const State& state, int transform) const {
    Key key;
    const bool swap_players = transform >= 4;
    auto add_pegs = [&](std::uint64_t pegs, std::uint64_t value) {
      while (pegs != 0) {
        const int cell = __builtin_ctzll(pegs);
        const int mapped = cell_transform_[transform][cell];
        const int bit = 2 * mapped;
        key.words[bit / 64] |= value << (bit % 64);
        pegs &= pegs - 1;
      }
    };
    add_pegs(state.white, swap_players ? 2 : 1);
    add_pegs(state.black, swap_players ? 1 : 2);
    for (int word = 0; word < link_word_count(); ++word) {
      std::uint64_t links = state.links[word];
      while (links != 0) {
        const int edge_id = 64 * word + __builtin_ctzll(links);
        const int mapped = edge_transform_[transform][edge_id];
        key.words[2 + mapped / 64] |= 1ULL << (mapped % 64);
        links &= links - 1;
      }
    }
    Player mapped_turn = state.turn;
    if (swap_players) mapped_turn = other(mapped_turn);
    if (mapped_turn == Player::Black) key.words.back() |= 1ULL << 63;
    return key;
  }
};

struct Child {
  int move = -1;
  int score = 0;
  State state;
  bool wins_immediately = false;
};

class Solver {
 public:
  Solver(const Twixt& game, Options options) : game_(game), options_(std::move(options)) {
    if (options_.tt_limit > 0) {
      table_.reserve(std::min<std::size_t>(options_.tt_limit, 4'000'000));
    }
  }

  Outcome solve(const State& state) {
    started_ = std::chrono::steady_clock::now();
    return search(state, 0);
  }

  std::vector<std::pair<int, Outcome>> root_moves(const State& state, Outcome target, bool all) {
    std::vector<std::pair<int, Outcome>> result;
    std::vector<Child> children = ordered_children(state);
    for (const Child& child : children) {
      Outcome value = child.wins_immediately ? Outcome::Win : negate(search(child.state, 1));
      if (value == target) {
        result.emplace_back(child.move, value);
        if (!all) break;
      }
    }
    return result;
  }

  std::vector<int> principal_variation(State state, Outcome value, int limit) {
    std::vector<int> pv;
    for (int ply = 0; ply < limit; ++ply) {
      if (game_.has_won(state, other(state.turn))) break;
      const std::vector<Child> children = ordered_children(state);
      bool found = false;
      for (const Child& child : children) {
        const Outcome child_value =
            child.wins_immediately ? Outcome::Win : negate(search(child.state, ply + 1));
        if (child_value != value) continue;
        pv.push_back(child.move);
        state = child.state;
        value = negate(value);
        found = true;
        break;
      }
      if (!found) break;
    }
    return pv;
  }

  const SearchStats& stats() const { return stats_; }
  std::size_t table_size() const { return table_.size(); }
  double elapsed_seconds() const {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count();
  }

 private:
  const Twixt& game_;
  Options options_;
  std::unordered_map<Key, std::int8_t, KeyHash> table_;
  SearchStats stats_;
  std::chrono::steady_clock::time_point started_{};

  void remember(const Key& key, Outcome value) {
    if (options_.tt_limit == 0 || table_.size() < options_.tt_limit) {
      table_.emplace(key, static_cast<std::int8_t>(value));
    } else {
      ++stats_.tt_dropped;
    }
  }

  Outcome search(const State& state, int depth) {
    ++stats_.nodes;
    stats_.max_depth = std::max(stats_.max_depth, static_cast<std::uint64_t>(depth));
    if (!options_.quiet && options_.progress_interval > 0 &&
        stats_.nodes % options_.progress_interval == 0) {
      const double seconds = elapsed_seconds();
      std::cerr << "nodes=" << stats_.nodes << " tt=" << table_.size() << " depth="
                << stats_.max_depth << " rate=" << static_cast<std::uint64_t>(stats_.nodes / std::max(0.001, seconds))
                << "/s\n";
    }

    const Key key = game_.canonical_key(state, options_.symmetry);
    const auto found = table_.find(key);
    if (found != table_.end()) {
      ++stats_.tt_hits;
      return static_cast<Outcome>(found->second);
    }

    if (game_.has_won(state, other(state.turn))) {
      remember(key, Outcome::Loss);
      return Outcome::Loss;
    }

    if (options_.draw_pruning && !game_.can_ever_win(state, Player::White) &&
        !game_.can_ever_win(state, Player::Black)) {
      ++stats_.draw_proofs;
      remember(key, Outcome::Draw);
      return Outcome::Draw;
    }

    std::vector<Child> children = ordered_children(state);
    if (children.empty()) {
      remember(key, Outcome::Draw);
      return Outcome::Draw;
    }

    bool found_draw = false;
    for (const Child& child : children) {
      Outcome value;
      if (child.wins_immediately) {
        ++stats_.immediate_wins;
        value = Outcome::Win;
      } else {
        value = negate(search(child.state, depth + 1));
      }
      if (value == Outcome::Win) {
        remember(key, Outcome::Win);
        return Outcome::Win;
      }
      if (value == Outcome::Draw) found_draw = true;
    }

    const Outcome result = found_draw ? Outcome::Draw : Outcome::Loss;
    remember(key, result);
    return result;
  }

  std::vector<Child> ordered_children(const State& state) const {
    std::vector<Child> children;
    const Player mover = state.turn;
    for (int move : game_.legal_moves(state)) {
      int links_added = 0;
      State next = game_.play(state, move, &links_added);
      const bool win = game_.has_won(next, mover);
      const int x = move % game_.width();
      const int y = move / game_.width();
      const int center_distance = std::abs(2 * x - (game_.width() - 1)) +
                                  std::abs(2 * y - (game_.height() - 1));
      int score = links_added * 100 - center_distance;
      if (win) score += 1'000'000;
      children.push_back(Child{move, score, next, win});
    }
    std::stable_sort(children.begin(), children.end(), [](const Child& lhs, const Child& rhs) {
      if (lhs.score != rhs.score) return lhs.score > rhs.score;
      return lhs.move < rhs.move;
    });
    return children;
  }
};

std::vector<std::string> split_moves(const std::string& text) {
  std::vector<std::string> result;
  std::string current;
  for (char ch : text) {
    if (ch == ',' || ch == ';' || std::isspace(static_cast<unsigned char>(ch))) {
      if (!current.empty()) {
        result.push_back(current);
        current.clear();
      }
    } else {
      current.push_back(ch);
    }
  }
  if (!current.empty()) result.push_back(current);
  return result;
}

std::size_t parse_size_t(const std::string& value, const char* option) {
  std::size_t used = 0;
  const unsigned long long parsed = std::stoull(value, &used);
  if (used != value.size()) throw std::invalid_argument(std::string("bad value for ") + option);
  return static_cast<std::size_t>(parsed);
}

double parse_double(const std::string& value, const char* option) {
  std::size_t used = 0;
  const double parsed = std::stod(value, &used);
  if (used != value.size() || parsed < 0.0) {
    throw std::invalid_argument(std::string("bad value for ") + option);
  }
  return parsed;
}

long double binomial(int n, int k) {
  if (k < 0 || k > n) return 0.0L;
  k = std::min(k, n - k);
  long double result = 1.0L;
  for (int i = 1; i <= k; ++i) {
    result *= static_cast<long double>(n - k + i) / i;
  }
  return result;
}

long double alternating_peg_diagrams(int width, int height) {
  const int white_exclusive = 2 * (width - 2);
  const int black_exclusive = 2 * (height - 2);
  const int common = (width - 2) * (height - 2);
  const int holes = width * height - 4;
  long double total = 0.0L;
  for (int white = 0; white <= (holes + 1) / 2; ++white) {
    for (int delta = 0; delta <= 1; ++delta) {
      const int black = white - delta;
      if (black < 0) continue;
      for (int we = 0; we <= white_exclusive; ++we) {
        const int wc = white - we;
        if (wc < 0 || wc > common) continue;
        for (int be = 0; be <= black_exclusive; ++be) {
          const int bc = black - be;
          if (bc < 0 || wc + bc > common) continue;
          total += binomial(white_exclusive, we) *
                   binomial(black_exclusive, be) * binomial(common, wc) *
                   binomial(common - wc, bc);
        }
      }
    }
  }
  return total;
}

std::string scientific(long double value) {
  std::ostringstream out;
  out << std::scientific << std::setprecision(3) << value;
  return out.str();
}

void print_estimate(const Twixt& game) {
  const int width = game.width();
  const int height = game.height();
  const int holes = width * height - 4;
  const int common = (width - 2) * (height - 2);
  const int white_exclusive = 2 * (width - 2);
  const int black_exclusive = 2 * (height - 2);
  const long double peg_diagrams = alternating_peg_diagrams(width, height);
  const long double loose_ceiling =
      std::ldexp(peg_diagrams, game.edge_count());
  State empty;
  std::cout << "Board estimate: " << width << 'x' << height << '\n'
            << "  playable holes: " << holes << '\n'
            << "  common / White-only / Black-only: " << common << " / "
            << white_exclusive << " / " << black_exclusive << '\n'
            << "  initial legal moves (White / Black): " << height * (width - 2)
            << " / " << width * (height - 2) << '\n'
            << "  knight-link edges: " << game.edge_count() << '\n'
            << "  board symmetries used: " << game.symmetry_count() << '\n'
            << "  alternating peg diagrams: " << scientific(peg_diagrams) << '\n'
            << "  peg diagrams / symmetry (optimistic): "
            // Diagonal/color swaps flip the turn and leave the white-first
            // reachable parity class, so at most four reachable orientations
            // merge even though square canonicalization considers eight images.
            << scientific(peg_diagrams / 4) << '\n'
            << "  loose peg+arbitrary-link ceiling: " << scientific(loose_ceiling)
            << '\n'
            << "  empty-board potential path (White / Black): "
            << (game.can_ever_win(empty, Player::White) ? "yes" : "no") << " / "
            << (game.can_ever_win(empty, Player::Black) ? "yes" : "no") << '\n';
}

struct PegKey {
  std::uint64_t a = 0;
  std::uint64_t b = 0;
  Player turn = Player::White;
  bool operator==(const PegKey& rhs) const {
    return a == rhs.a && b == rhs.b && turn == rhs.turn;
  }
};

struct PegKeyHash {
  std::size_t operator()(const PegKey& key) const noexcept {
    return static_cast<std::size_t>(mix64(key.a) ^
                                    mix64(key.b + static_cast<int>(key.turn)));
  }
};

struct DfaVariable {
  bool link = false;
  int index = 0;
};

// Four words hold 128 two-bit symbols, enough for the 5x6 and 6x6 pilot.
struct DfaWord {
  std::array<std::uint64_t, 4> words{};
  bool operator<(const DfaWord& rhs) const { return words < rhs.words; }
  bool operator==(const DfaWord& rhs) const { return words == rhs.words; }

  void set(int position, std::uint8_t symbol) {
    const int shift = 62 - 2 * (position % 32);
    words[position / 32] |= static_cast<std::uint64_t>(symbol) << shift;
  }
  std::uint8_t get(int position) const {
    const int shift = 62 - 2 * (position % 32);
    return static_cast<std::uint8_t>((words[position / 32] >> shift) & 3ULL);
  }
};

struct DfaNodeSignature {
  std::array<std::int32_t, 3> next{{-1, -1, -1}};
  bool final = false;
  bool operator==(const DfaNodeSignature& rhs) const {
    return next == rhs.next && final == rhs.final;
  }
};

struct DfaNodeHash {
  std::size_t operator()(const DfaNodeSignature& node) const noexcept {
    std::uint64_t hash = node.final ? 0x9e3779b97f4a7c15ULL
                                    : 0x243f6a8885a308d3ULL;
    for (std::int32_t child : node.next) {
      hash ^= mix64(static_cast<std::uint32_t>(child) + hash);
    }
    return static_cast<std::size_t>(hash);
  }
};

struct DfaStats {
  std::uint64_t states = 0;
  std::uint64_t arcs = 0;
};

std::vector<DfaVariable> dfa_variables(const Twixt& game, bool frontier,
                                       bool pilot_limit = true) {
  std::vector<DfaVariable> variables;
  if (!frontier) {
    for (int cell = 0; cell < game.cell_count(); ++cell) {
      if (game.valid_cell(cell)) variables.push_back({false, cell});
    }
    for (int edge = 0; edge < game.edge_count(); ++edge) {
      variables.push_back({true, edge});
    }
  } else {
    for (int cell = 0; cell < game.cell_count(); ++cell) {
      if (!game.valid_cell(cell)) continue;
      variables.push_back({false, cell});
      for (int edge = 0; edge < game.edge_count(); ++edge) {
        const Edge& endpoints = game.edge(edge);
        if (std::max<int>(endpoints.a, endpoints.b) == cell) {
          variables.push_back({true, edge});
        }
      }
    }
  }
  // Square-board diagonal symmetries can swap the canonical side to move.
  variables.push_back({false, -1});
  if (pilot_limit && variables.size() > 128) {
    throw std::invalid_argument(
        "--dfa-stats currently supports position strings of at most 128 symbols");
  }
  return variables;
}

DfaWord make_dfa_word(const Twixt& game, const Key& key,
                      const std::vector<DfaVariable>& variables) {
  const State state = game.unpack_key(key);
  DfaWord word;
  for (std::size_t position = 0; position < variables.size(); ++position) {
    const DfaVariable variable = variables[position];
    std::uint8_t symbol = 0;
    if (variable.index < 0) {
      symbol = state.turn == Player::Black ? 1 : 0;
    } else if (variable.link) {
      symbol = static_cast<std::uint8_t>(
          (state.links[variable.index / 64] >> (variable.index % 64)) & 1ULL);
    } else {
      symbol = static_cast<std::uint8_t>(
          ((state.white >> variable.index) & 1ULL) != 0
              ? 1
              : ((state.black >> variable.index) & 1ULL) != 0 ? 2 : 0);
    }
    word.set(static_cast<int>(position), symbol);
  }
  return word;
}

DfaStats minimal_acyclic_dfa(std::vector<DfaWord>& words, int length) {
  std::sort(words.begin(), words.end());
  words.erase(std::unique(words.begin(), words.end()), words.end());

  std::vector<DfaNodeSignature> nodes(1);
  std::vector<int> free_nodes;
  std::vector<int> path(length + 1, -1);
  path[0] = 0;
  std::unordered_map<DfaNodeSignature, int, DfaNodeHash> registry;
  registry.reserve(std::max<std::size_t>(1024, words.size() / 4));
  DfaStats stats;

  auto allocate = [&]() {
    if (!free_nodes.empty()) {
      const int id = free_nodes.back();
      free_nodes.pop_back();
      nodes[id] = DfaNodeSignature{};
      return id;
    }
    nodes.emplace_back();
    return static_cast<int>(nodes.size() - 1);
  };
  auto register_node = [&](int id) {
    const auto [found, inserted] = registry.emplace(nodes[id], id);
    if (!inserted) {
      free_nodes.push_back(id);
      return found->second;
    }
    ++stats.states;
    for (int child : nodes[id].next) {
      if (child >= 0) ++stats.arcs;
    }
    return id;
  };

  DfaWord previous;
  bool have_previous = false;
  for (const DfaWord& word : words) {
    int common = 0;
    if (have_previous) {
      while (common < length && word.get(common) == previous.get(common)) {
        ++common;
      }
      for (int depth = length; depth > common; --depth) {
        const int child = register_node(path[depth]);
        nodes[path[depth - 1]].next[previous.get(depth - 1)] = child;
      }
    }
    for (int depth = common; depth < length; ++depth) {
      const int child = allocate();
      nodes[path[depth]].next[word.get(depth)] = child;
      path[depth + 1] = child;
    }
    nodes[path[length]].final = true;
    previous = word;
    have_previous = true;
  }
  if (have_previous) {
    for (int depth = length; depth > 0; --depth) {
      const int child = register_node(path[depth]);
      nodes[path[depth - 1]].next[previous.get(depth - 1)] = child;
    }
    (void)register_node(path[0]);
  }
  return stats;
}

void print_dfa_stats(const Twixt& game, const std::vector<Key>& layer, int ply,
                     bool frontier) {
  using Clock = std::chrono::steady_clock;
  const auto started = Clock::now();
  const std::vector<DfaVariable> variables = dfa_variables(game, frontier);
  std::vector<DfaWord> words;
  words.reserve(layer.size());
  for (const Key& key : layer) words.push_back(make_dfa_word(game, key, variables));
  const DfaStats stats =
      minimal_acyclic_dfa(words, static_cast<int>(variables.size()));
  const double seconds =
      std::chrono::duration<double>(Clock::now() - started).count();
  const double positions_per_state =
      stats.states == 0 ? 0.0 : static_cast<double>(layer.size()) / stats.states;
  std::cout << "dfa," << (frontier ? "frontier" : "linear") << ',' << ply
            << ',' << layer.size() << ',' << variables.size() << ','
            << stats.states << ',' << stats.arcs << ',' << std::fixed
            << std::setprecision(3) << positions_per_state << ',' << seconds
            << '\n';
}

bool run_census(const Twixt& game, const State& root, const Options& options) {
  using Clock = std::chrono::steady_clock;
  const auto started = Clock::now();
  std::vector<Key> layer{game.canonical_key(root, options.symmetry)};

  std::cout << "Reachable-state census (bounded, full peg+link states)\n"
            << "ply,states,peg_diagrams,link_multiplicity,raw_children,terminal_wins,"
               "terminal_draws,no_moves\n";
  if (options.dfa_stats) {
    std::cout << "DFA statistics\n"
              << "kind,order,ply,positions,symbols,dfa_states,dfa_arcs,"
                 "positions_per_dfa_state,seconds\n";
  }

  for (int ply = 0; ply <= options.census_ply; ++ply) {
    std::unordered_set<PegKey, PegKeyHash> peg_keys;
    peg_keys.reserve(layer.size());
    std::unordered_set<Key, KeyHash> next;
    if (options.census_max_states > 0) {
      next.reserve(std::min<std::size_t>(options.census_max_states,
                                         std::max<std::size_t>(1024, layer.size() * 4)));
    }
    std::uint64_t raw_children = 0;
    std::uint64_t terminal_wins = 0;
    std::uint64_t terminal_draws = 0;
    std::uint64_t no_moves = 0;
    bool capped = false;
    std::string cap_reason;

    for (const Key& key : layer) {
      const State state = game.unpack_key(key);
      State peg_state = state;
      peg_state.links.fill(0);
      const Key canonical_pegs =
          game.canonical_key(peg_state, options.symmetry);
      const State canonical_peg_state = game.unpack_key(canonical_pegs);
      peg_keys.insert(PegKey{canonical_pegs.words[0], canonical_pegs.words[1],
                             canonical_peg_state.turn});

      if (game.has_won(state, other(state.turn))) {
        ++terminal_wins;
        continue;
      }
      if (options.draw_pruning &&
          !game.can_ever_win(state, Player::White) &&
          !game.can_ever_win(state, Player::Black)) {
        ++terminal_draws;
        continue;
      }
      const std::vector<int> moves = game.legal_moves(state);
      if (moves.empty()) {
        ++no_moves;
        continue;
      }
      if (ply == options.census_ply) continue;

      const CanonicalContext canonical =
          game.prepare_canonical(state, options.symmetry);
      for (int move : moves) {
        const State child = game.play(state, move);
        next.insert(game.canonical_child_key(canonical, state, child, move));
        ++raw_children;
        if (options.census_max_states > 0 &&
            next.size() > options.census_max_states) {
          capped = true;
          cap_reason = "state cap";
          break;
        }
        if ((raw_children & 0x3fffULL) == 0 && options.census_seconds > 0.0 &&
            std::chrono::duration<double>(Clock::now() - started).count() >=
                options.census_seconds) {
          capped = true;
          cap_reason = "time cap";
          break;
        }
      }
      if (capped) break;
    }

    const double multiplicity =
        peg_keys.empty() ? 0.0
                         : static_cast<double>(layer.size()) / peg_keys.size();
    if (capped) {
      // The layer is complete, but only a prefix was inspected.  Dividing the
      // full layer by this partial peg count produces a spurious multiplicity.
      std::cout << ply << ',' << layer.size() << ",NA,NA," << raw_children
                << ",NA,NA,NA\n";
    } else {
      std::cout << ply << ',' << layer.size() << ',' << peg_keys.size() << ','
                << std::fixed << std::setprecision(4) << multiplicity << ','
                << raw_children << ',' << terminal_wins << ',' << terminal_draws
                << ',' << no_moves << '\n';
    }

    if (options.dfa_stats) {
      print_dfa_stats(game, layer, ply, false);
      print_dfa_stats(game, layer, ply, true);
    }

    if (capped) {
      std::cout << "Census stopped before completing ply " << ply + 1 << " ("
                << cap_reason << "). Counts for the partial next layer are not reported.\n";
      return false;
    }
    if (ply == options.census_ply) break;
    layer.assign(next.begin(), next.end());
    if (layer.empty()) break;
  }
  const double seconds =
      std::chrono::duration<double>(Clock::now() - started).count();
  std::cout << "Census complete through requested ply in " << std::fixed
            << std::setprecision(3) << seconds << " s.\n";
  return true;
}

struct StrongSolution {
  std::vector<std::vector<Key>> layers;
  std::vector<std::vector<std::int8_t>> values;
  Outcome root = Outcome::Draw;
  std::uint64_t total_states = 0;
};

std::string absolute_result(const State& root, Outcome outcome);

std::optional<Outcome> terminal_outcome(const Twixt& game, const State& state,
                                        bool draw_pruning) {
  if (game.has_won(state, other(state.turn))) return Outcome::Loss;
  if (draw_pruning && !game.can_ever_win(state, Player::White) &&
      !game.can_ever_win(state, Player::Black)) {
    return Outcome::Draw;
  }
  if (game.legal_moves(state).empty()) return Outcome::Draw;
  return std::nullopt;
}

bool strong_time_expired(const Options& options,
                         std::chrono::steady_clock::time_point started) {
  return options.census_seconds > 0.0 &&
         std::chrono::duration<double>(std::chrono::steady_clock::now() - started)
                 .count() >= options.census_seconds;
}

std::optional<StrongSolution> build_strong_solution(const Twixt& game,
                                                    const State& root,
                                                    const Options& options) {
  using Clock = std::chrono::steady_clock;
  const auto started = Clock::now();
  StrongSolution solution;
  solution.layers.push_back(
      std::vector<Key>{game.canonical_key(root, options.symmetry)});

  std::cout << "Strong forward enumeration (full reachable peg+link states)\n";
  for (int ply = 0;; ++ply) {
    const std::vector<Key>& layer = solution.layers.back();
    solution.total_states += layer.size();
    std::cout << "  ply " << ply << ": " << layer.size() << " states\n";

    std::unordered_set<Key, KeyHash> next;
    if (options.census_max_states > 0) {
      next.reserve(std::min<std::size_t>(options.census_max_states,
                                         std::max<std::size_t>(1024, layer.size() * 4)));
    }
    std::uint64_t generated = 0;
    for (const Key& key : layer) {
      const State state = game.unpack_key(key);
      if (terminal_outcome(game, state, options.draw_pruning).has_value()) {
        continue;
      }
      const CanonicalContext canonical =
          game.prepare_canonical(state, options.symmetry);
      for (int move : game.legal_moves(state)) {
        const State child = game.play(state, move);
        next.insert(game.canonical_child_key(canonical, state, child, move));
        ++generated;
        if (options.census_max_states > 0 &&
            next.size() > options.census_max_states) {
          std::cout << "Strong enumeration stopped: ply " << ply + 1
                    << " exceeded the " << options.census_max_states
                    << " state cap.\n";
          return std::nullopt;
        }
        if ((generated & 0x3fffULL) == 0 &&
            strong_time_expired(options, started)) {
          std::cout << "Strong enumeration stopped: wall-time cap reached.\n";
          return std::nullopt;
        }
      }
    }
    if (next.empty()) break;
    std::vector<Key> next_layer(next.begin(), next.end());
    std::sort(next_layer.begin(), next_layer.end());
    solution.layers.push_back(std::move(next_layer));
  }

  solution.values.resize(solution.layers.size());
  std::cout << "Strong retrograde evaluation\n";
  std::uint64_t evaluated = 0;
  for (std::size_t reverse = solution.layers.size(); reverse-- > 0;) {
    const std::vector<Key>& layer = solution.layers[reverse];
    std::vector<std::int8_t>& values = solution.values[reverse];
    values.resize(layer.size());
    for (std::size_t index = 0; index < layer.size(); ++index) {
      const State state = game.unpack_key(layer[index]);
      if (const std::optional<Outcome> terminal =
              terminal_outcome(game, state, options.draw_pruning);
          terminal.has_value()) {
        values[index] = static_cast<std::int8_t>(*terminal);
      } else {
        if (reverse + 1 >= solution.layers.size()) {
          throw std::logic_error("nonterminal state in final strong layer");
        }
        const std::vector<Key>& children = solution.layers[reverse + 1];
        const std::vector<std::int8_t>& child_values =
            solution.values[reverse + 1];
        Outcome best = Outcome::Loss;
        const CanonicalContext canonical =
            game.prepare_canonical(state, options.symmetry);
        for (int move : game.legal_moves(state)) {
          const State child_state = game.play(state, move);
          const Key child = game.canonical_child_key(
              canonical, state, child_state, move);
          const auto found = std::lower_bound(children.begin(), children.end(), child);
          if (found == children.end() || !(*found == child)) {
            throw std::logic_error("strong child missing from next layer");
          }
          const std::size_t child_index =
              static_cast<std::size_t>(found - children.begin());
          const Outcome value =
              negate(static_cast<Outcome>(child_values[child_index]));
          if (value == Outcome::Win) {
            best = Outcome::Win;
            break;
          }
          if (value == Outcome::Draw) best = Outcome::Draw;
        }
        values[index] = static_cast<std::int8_t>(best);
      }
      if (((++evaluated) & 0x3fffULL) == 0 &&
          strong_time_expired(options, started)) {
        std::cout << "Strong retrograde stopped: wall-time cap reached.\n";
        return std::nullopt;
      }
    }
    std::cout << "  ply " << reverse << ": " << layer.size()
              << " values resolved\n";
  }
  solution.root = static_cast<Outcome>(solution.values[0][0]);
  return solution;
}

void write_u32(std::ostream& out, std::uint32_t value) {
  for (int byte = 0; byte < 4; ++byte) {
    out.put(static_cast<char>((value >> (8 * byte)) & 0xffU));
  }
}

void write_u64(std::ostream& out, std::uint64_t value) {
  for (int byte = 0; byte < 8; ++byte) {
    out.put(static_cast<char>((value >> (8 * byte)) & 0xffULL));
  }
}

std::uint32_t read_u32(std::istream& in) {
  std::uint32_t value = 0;
  for (int byte = 0; byte < 4; ++byte) {
    const int ch = in.get();
    if (ch == std::char_traits<char>::eof()) {
      throw std::runtime_error("truncated WDL database header");
    }
    value |= static_cast<std::uint32_t>(static_cast<unsigned char>(ch))
             << (8 * byte);
  }
  return value;
}

std::uint64_t read_u64(std::istream& in) {
  std::uint64_t value = 0;
  for (int byte = 0; byte < 8; ++byte) {
    const int ch = in.get();
    if (ch == std::char_traits<char>::eof()) {
      throw std::runtime_error("truncated WDL database");
    }
    value |= static_cast<std::uint64_t>(static_cast<unsigned char>(ch))
             << (8 * byte);
  }
  return value;
}

class WdlDatabase {
 public:
  explicit WdlDatabase(const std::string& path)
      : input_(path, std::ios::binary) {
    if (!input_) throw std::runtime_error("cannot open WDL database: " + path);
    char magic[8]{};
    input_.read(magic, sizeof(magic));
    if (input_.gcount() != static_cast<std::streamsize>(sizeof(magic)) ||
        std::string(magic, sizeof(magic)) != "TWIXTDB1") {
      throw std::runtime_error("not a TWIXTDB1 database: " + path);
    }
    const std::uint32_t version = read_u32(input_);
    if (version != 1) throw std::runtime_error("unsupported WDL database version");
    width_ = static_cast<int>(read_u32(input_));
    height_ = static_cast<int>(read_u32(input_));
    symmetry_ = read_u32(input_) != 0;
    draw_pruning_ = read_u32(input_) != 0;
    key_words_ = static_cast<int>(read_u32(input_));
    const std::uint32_t layer_count = read_u32(input_);
    total_states_ = read_u64(input_);
    if (layer_count == 0 || key_words_ <= 0 || key_words_ > 2 + kLinkWords) {
      throw std::runtime_error("invalid WDL database dimensions");
    }
    root_compact_.fill(0);
    for (int word = 0; word < key_words_; ++word) {
      root_compact_[word] = read_u64(input_);
    }
    layer_counts_.resize(layer_count);
    for (std::uint64_t& count : layer_counts_) count = read_u64(input_);
    const std::uint64_t record_bytes = static_cast<std::uint64_t>(8 * key_words_ + 1);
    const std::uint64_t data_offset =
        static_cast<std::uint64_t>(input_.tellg());
    layer_offsets_.resize(layer_count);
    std::uint64_t offset = data_offset;
    std::uint64_t counted = 0;
    for (std::size_t layer = 0; layer < layer_counts_.size(); ++layer) {
      layer_offsets_[layer] = offset;
      if (layer_counts_[layer] >
          (std::numeric_limits<std::uint64_t>::max() - offset) / record_bytes) {
        throw std::runtime_error("WDL database offset overflow");
      }
      offset += layer_counts_[layer] * record_bytes;
      counted += layer_counts_[layer];
    }
    if (counted != total_states_) {
      throw std::runtime_error("WDL database state count mismatch");
    }
    input_.seekg(0, std::ios::end);
    const std::uint64_t file_bytes = static_cast<std::uint64_t>(input_.tellg());
    if (file_bytes != offset) throw std::runtime_error("WDL database size mismatch");
  }

  int width() const { return width_; }
  int height() const { return height_; }
  bool symmetry() const { return symmetry_; }
  bool draw_pruning() const { return draw_pruning_; }
  std::uint64_t total_states() const { return total_states_; }
  std::size_t layer_count() const { return layer_counts_.size(); }

  Key root_key(const Twixt& game) const {
    verify_game(game);
    return game.expand_compact_key(root_compact_);
  }

  Outcome lookup(const Twixt& game, std::size_t layer, const Key& key) {
    verify_game(game);
    if (layer >= layer_counts_.size()) {
      throw std::invalid_argument("position is beyond the WDL database layers");
    }
    std::uint64_t low = 0;
    std::uint64_t high = layer_counts_[layer];
    while (low < high) {
      const std::uint64_t middle = low + (high - low) / 2;
      const auto [candidate, value] = read_record(layer, middle, game);
      if (candidate < key) {
        low = middle + 1;
      } else if (key < candidate) {
        high = middle;
      } else {
        return value;
      }
    }
    throw std::invalid_argument("position is not present in the WDL database");
  }

 private:
  void verify_game(const Twixt& game) const {
    if (game.width() != width_ || game.height() != height_ ||
        game.compact_key_word_count() != key_words_) {
      throw std::runtime_error("WDL database/game geometry mismatch");
    }
  }

  std::pair<Key, Outcome> read_record(std::size_t layer, std::uint64_t index,
                                      const Twixt& game) {
    const std::uint64_t record_bytes = static_cast<std::uint64_t>(8 * key_words_ + 1);
    const std::uint64_t offset = layer_offsets_[layer] + index * record_bytes;
    input_.clear();
    input_.seekg(static_cast<std::streamoff>(offset));
    if (!input_) throw std::runtime_error("WDL database seek failed");
    std::array<std::uint64_t, 2 + kLinkWords> compact{};
    for (int word = 0; word < key_words_; ++word) compact[word] = read_u64(input_);
    const int raw_value = input_.get();
    if (raw_value == std::char_traits<char>::eof()) {
      throw std::runtime_error("truncated WDL database record");
    }
    const std::int8_t signed_value =
        static_cast<std::int8_t>(static_cast<unsigned char>(raw_value));
    if (signed_value < -1 || signed_value > 1) {
      throw std::runtime_error("invalid WDL value byte");
    }
    return {game.expand_compact_key(compact),
            static_cast<Outcome>(signed_value)};
  }

  std::ifstream input_;
  int width_ = 0;
  int height_ = 0;
  bool symmetry_ = false;
  bool draw_pruning_ = false;
  int key_words_ = 0;
  std::uint64_t total_states_ = 0;
  std::array<std::uint64_t, 2 + kLinkWords> root_compact_{};
  std::vector<std::uint64_t> layer_counts_;
  std::vector<std::uint64_t> layer_offsets_;
};

void save_strong_database(const Twixt& game, const State& root,
                          const Options& options,
                          const StrongSolution& solution) {
  std::ofstream out(options.db_out, std::ios::binary | std::ios::trunc);
  if (!out) throw std::runtime_error("cannot open database output: " + options.db_out);
  const char magic[8] = {'T', 'W', 'I', 'X', 'T', 'D', 'B', '1'};
  out.write(magic, sizeof(magic));
  write_u32(out, 1);
  write_u32(out, static_cast<std::uint32_t>(game.width()));
  write_u32(out, static_cast<std::uint32_t>(game.height()));
  write_u32(out, options.symmetry ? 1U : 0U);
  write_u32(out, options.draw_pruning ? 1U : 0U);
  write_u32(out, static_cast<std::uint32_t>(game.compact_key_word_count()));
  write_u32(out, static_cast<std::uint32_t>(solution.layers.size()));
  write_u64(out, solution.total_states);

  const Key root_key = game.canonical_key(root, options.symmetry);
  const auto compact_root = game.compact_key(root_key);
  for (int word = 0; word < game.compact_key_word_count(); ++word) {
    write_u64(out, compact_root[word]);
  }
  for (const std::vector<Key>& layer : solution.layers) {
    write_u64(out, layer.size());
  }
  for (std::size_t ply = 0; ply < solution.layers.size(); ++ply) {
    for (std::size_t index = 0; index < solution.layers[ply].size(); ++index) {
      const auto compact = game.compact_key(solution.layers[ply][index]);
      for (int word = 0; word < game.compact_key_word_count(); ++word) {
        write_u64(out, compact[word]);
      }
      const std::int8_t value = solution.values[ply][index];
      out.write(reinterpret_cast<const char*>(&value), sizeof(value));
    }
  }
  if (!out) throw std::runtime_error("failed while writing database: " + options.db_out);
  std::cout << "Strong database written to " << options.db_out << '\n';
}

bool run_strong(const Twixt& game, const State& root, const Options& options) {
  const std::optional<StrongSolution> solution =
      build_strong_solution(game, root, options);
  if (!solution.has_value()) return false;
  const std::string result = absolute_result(root, solution->root);
  std::cout << "Strong result: "
            << (result == "draw" ? "Draw"
                                  : result == "white" ? "White win" : "Black win")
            << "\nStrong database states: " << solution->total_states << '\n';
  if (!options.db_out.empty()) {
    save_strong_database(game, root, options, *solution);
  }
  if (options.expected.has_value() && *options.expected != result) {
    std::cerr << "expected " << *options.expected << " but obtained " << result
              << '\n';
    return false;
  }
  return true;
}

int peg_count(const State& state) {
  return __builtin_popcountll(state.white) + __builtin_popcountll(state.black);
}

void run_database_probe(const Twixt& game, const State& state,
                        WdlDatabase& database, bool quiet) {
  const State database_root = game.unpack_key(database.root_key(game));
  const int relative_ply = peg_count(state) - peg_count(database_root);
  if (relative_ply < 0 ||
      static_cast<std::size_t>(relative_ply) >= database.layer_count()) {
    throw std::invalid_argument("position ply is outside the WDL database");
  }
  const Key key = game.canonical_key(state, database.symmetry());
  const Outcome value =
      database.lookup(game, static_cast<std::size_t>(relative_ply), key);
  if (!quiet) game.print_board(state, std::cout);
  std::cout << "WDL (" << player_name(state.turn) << " to move): ";
  if (value == Outcome::Win) std::cout << "WIN\n";
  if (value == Outcome::Draw) std::cout << "DRAW\n";
  if (value == Outcome::Loss) std::cout << "LOSS\n";
  std::cout << "Database: " << game.width() << 'x' << game.height() << ", "
            << database.total_states() << " states, " << database.layer_count()
            << " layers\n";

  if (terminal_outcome(game, state, database.draw_pruning()).has_value()) return;
  std::cout << "Moves:";
  for (int move : game.legal_moves(state)) {
    const State child = game.play(state, move);
    const Key child_key = game.canonical_key(child, database.symmetry());
    const Outcome move_value = negate(database.lookup(
        game, static_cast<std::size_t>(relative_ply + 1), child_key));
    std::cout << ' ' << game.coordinate(move) << '=';
    if (move_value == Outcome::Win) std::cout << 'W';
    if (move_value == Outcome::Draw) std::cout << 'D';
    if (move_value == Outcome::Loss) std::cout << 'L';
    if (move_value == value) std::cout << '*';
  }
  std::cout << '\n';
}

#include "twixt_symbolic.hpp"

void print_help(const char* program) {
  std::cout
      << "Usage: " << program << " [options]\n\n"
      << "Exactly analyze auto-link computer TwixT on square or rectangular boards.\n\n"
      << "  --size N              set a square N x N board (default: 5)\n"
      << "  --board WxH           set rectangular dimensions, e.g. 5x7\n"
      << "  --width N             set board width\n"
      << "  --height N            set board height\n"
      << "  --moves LIST          replay A1-style moves, separated by comma/space\n"
      << "  --all-optimal         print every game-theoretically optimal root move\n"
      << "  --pv-length N         maximum principal-variation moves to print\n"
      << "  --tt-limit N          maximum exact TT entries; 0 is unlimited\n"
      << "  --progress N          report every N searched nodes; 0 disables\n"
      << "  --no-symmetry         disable dihedral/color symmetry reduction\n"
      << "  --no-draw-pruning     disable potential-connection draw proofs\n"
      << "  --estimate            print a combinatorial board estimate and exit\n"
      << "  --census-ply N        enumerate exact reachable states through ply N\n"
      << "  --dfa-stats           minimize census layers as DFA languages\n"
      << "  --symbolic            solve with DFA move generation and retrograde\n"
      << "  --symbolic-ply N      generate DFA reachable layers only, through ply N\n"
      << "  --symbolic-verify     compare a symbolic solve/probe to explicit analysis\n"
      << "  --dfa-max-nodes N     cap shared DFA pool including intermediates (default: 5M)\n"
      << "  --workers N           parallel symbolic workers, 1..64 (default: 1)\n"
      << "  --parallel-backend B  threads (default) or processes\n"
      << "  --parallel-min-nodes N use workers when active DFA has at least N nodes (default: 20000)\n"
      << "  --parallel-memory-gb N aggregate worker PSS cap in GiB; 0 disables (default: 512)\n"
      << "  --parallel-temp-dir PATH temporary worker deltas (default: tables)\n"
      << "  --dfa-out PATH        save a completed symbolic WDL solution\n"
      << "  --probe-dfa PATH      query a symbolic solution at --moves\n"
      << "  --compare-dfa PATH    compare exact languages with a solution/forward checkpoint\n"
      << "  --dfa-checkpoint PATH atomically checkpoint each symbolic layer\n"
      << "  --resume-dfa PATH     resume a checkpoint (uses a fresh time budget)\n"
      << "  --census-max-states N stop before a layer exceeds N states (default: 1M)\n"
      << "  --census-seconds S    census/strong/symbolic time cap; 0 disables (default: 10)\n"
      << "  --strong              enumerate and retrograde-solve every reachable state\n"
      << "  --db-out PATH         write a completed --strong database\n"
      << "  --probe-db PATH       query a WDL database at --moves (empty = root)\n"
      << "  --expect RESULT       exit unsuccessfully unless result is white/black/draw\n"
      << "  --quiet               suppress board, PV, and progress output\n"
      << "  --self-test           run geometry/rules tests and exit\n"
      << "  --help                show this help\n";
}

Options parse_options(int argc, char** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto value = [&](const char* name) -> std::string {
      if (i + 1 >= argc) throw std::invalid_argument(std::string("missing value after ") + name);
      return argv[++i];
    };
    if (arg == "--size") {
      const int size = static_cast<int>(parse_size_t(value("--size"), "--size"));
      options.width = options.height = size;
    } else if (arg == "--board") {
      const std::string board = value("--board");
      const std::size_t separator = board.find_first_of("xX");
      if (separator == std::string::npos) {
        throw std::invalid_argument("--board must use WxH, for example 5x7");
      }
      options.width = static_cast<int>(parse_size_t(board.substr(0, separator), "--board"));
      options.height =
          static_cast<int>(parse_size_t(board.substr(separator + 1), "--board"));
    } else if (arg == "--width") {
      options.width = static_cast<int>(parse_size_t(value("--width"), "--width"));
    } else if (arg == "--height") {
      options.height = static_cast<int>(parse_size_t(value("--height"), "--height"));
    } else if (arg == "--moves") {
      options.moves = value("--moves");
    } else if (arg == "--tt-limit") {
      options.tt_limit = parse_size_t(value("--tt-limit"), "--tt-limit");
    } else if (arg == "--progress") {
      options.progress_interval = parse_size_t(value("--progress"), "--progress");
    } else if (arg == "--pv-length") {
      options.pv_length = static_cast<int>(parse_size_t(value("--pv-length"), "--pv-length"));
    } else if (arg == "--expect") {
      options.expected = value("--expect");
      std::transform(options.expected->begin(), options.expected->end(), options.expected->begin(),
                     [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    } else if (arg == "--no-symmetry") {
      options.symmetry = false;
    } else if (arg == "--no-draw-pruning") {
      options.draw_pruning = false;
    } else if (arg == "--estimate") {
      options.estimate = true;
    } else if (arg == "--census-ply") {
      options.census_ply =
          static_cast<int>(parse_size_t(value("--census-ply"), "--census-ply"));
    } else if (arg == "--dfa-stats") {
      options.dfa_stats = true;
    } else if (arg == "--symbolic") {
      options.symbolic = true;
    } else if (arg == "--symbolic-ply") {
      options.symbolic = true;
      options.symbolic_ply = static_cast<int>(parse_size_t(value("--symbolic-ply"), "--symbolic-ply"));
    } else if (arg == "--symbolic-verify") {
      options.symbolic_verify = true;
    } else if (arg == "--dfa-max-nodes") {
      options.dfa_max_nodes = parse_size_t(value("--dfa-max-nodes"), "--dfa-max-nodes");
    } else if (arg == "--workers") {
      const auto count = parse_size_t(value("--workers"), "--workers");
      if (count == 0 || count > 64) throw std::invalid_argument("--workers must be 1..64");
      options.workers = static_cast<unsigned>(count);
    } else if (arg == "--parallel-min-nodes") {
      options.parallel_min_nodes = parse_size_t(value("--parallel-min-nodes"), "--parallel-min-nodes");
    } else if (arg == "--parallel-memory-gb") {
      options.parallel_memory_gb = parse_size_t(value("--parallel-memory-gb"), "--parallel-memory-gb");
      if (options.parallel_memory_gb > std::numeric_limits<std::uint64_t>::max() / (1ULL << 30)) {
        throw std::invalid_argument("--parallel-memory-gb is too large");
      }
    } else if (arg == "--parallel-temp-dir") {
      options.parallel_temp_directory = value("--parallel-temp-dir");
    } else if (arg == "--parallel-backend") {
      const std::string backend = value("--parallel-backend");
      if (backend != "threads" && backend != "processes") {
        throw std::invalid_argument("--parallel-backend must be threads or processes");
      }
      options.parallel_processes = backend == "processes";
    } else if (arg == "--dfa-out") {
      options.dfa_out = value("--dfa-out");
    } else if (arg == "--probe-dfa") {
      options.probe_dfa = value("--probe-dfa");
    } else if (arg == "--compare-dfa") {
      options.compare_dfa = value("--compare-dfa");
    } else if (arg == "--dfa-checkpoint") {
      options.dfa_checkpoint = value("--dfa-checkpoint");
    } else if (arg == "--resume-dfa") {
      options.symbolic = true;
      options.resume_dfa = value("--resume-dfa");
    } else if (arg == "--census-max-states") {
      options.census_max_states =
          parse_size_t(value("--census-max-states"), "--census-max-states");
    } else if (arg == "--census-seconds") {
      options.census_seconds =
          parse_double(value("--census-seconds"), "--census-seconds");
    } else if (arg == "--strong") {
      options.strong = true;
    } else if (arg == "--db-out") {
      options.db_out = value("--db-out");
    } else if (arg == "--probe-db") {
      options.probe_db = value("--probe-db");
    } else if (arg == "--all-optimal") {
      options.list_optimal = true;
    } else if (arg == "--quiet") {
      options.quiet = true;
      options.progress_interval = 0;
    } else if (arg == "--self-test") {
      options.self_test = true;
    } else if (arg == "--help" || arg == "-h") {
      print_help(argv[0]);
      std::exit(0);
    } else {
      throw std::invalid_argument("unknown option: " + arg);
    }
  }
  return options;
}

std::string absolute_result(const State& root, Outcome outcome) {
  if (outcome == Outcome::Draw) return "draw";
  const Player winner = outcome == Outcome::Win ? root.turn : other(root.turn);
  return winner == Player::White ? "white" : "black";
}

int run(int argc, char** argv) {
  Options options = parse_options(argc, argv);
  const int analysis_modes = static_cast<int>(options.estimate) +
                             static_cast<int>(options.census_ply >= 0) +
                             static_cast<int>(options.strong) +
                             static_cast<int>(options.symbolic) +
                             static_cast<int>(!options.probe_dfa.empty()) +
                             static_cast<int>(!options.probe_db.empty());
  if (analysis_modes > 1) {
    throw std::invalid_argument(
        "estimate, census, explicit, symbolic, and probe modes are mutually exclusive");
  }
  if (!options.db_out.empty() && !options.strong) {
    throw std::invalid_argument("--db-out requires --strong");
  }
  if (options.dfa_stats && options.census_ply < 0) {
    throw std::invalid_argument("--dfa-stats requires --census-ply");
  }
  if (options.symbolic_verify && !options.symbolic && options.probe_dfa.empty()) {
    throw std::invalid_argument("--symbolic-verify requires a symbolic solve or --probe-dfa");
  }
  if (!options.dfa_out.empty() && (!options.symbolic || options.symbolic_ply >= 0)) {
    throw std::invalid_argument("--dfa-out requires a full --symbolic solve");
  }
  if (!options.dfa_checkpoint.empty() && !options.symbolic) {
    throw std::invalid_argument("--dfa-checkpoint requires symbolic solving or generation");
  }
  if (!options.resume_dfa.empty() && !options.moves.empty()) {
    throw std::invalid_argument("--resume-dfa uses the saved root; --moves cannot change it");
  }
  if (!options.dfa_out.empty() && options.dfa_out == options.dfa_checkpoint) {
    throw std::invalid_argument("solution and checkpoint paths must differ");
  }
  if (options.symbolic_ply >= 0 && options.expected.has_value()) {
    throw std::invalid_argument("--expect requires a complete solve, not --symbolic-ply");
  }
  if (options.workers > 1 && !options.symbolic) {
    throw std::invalid_argument("--workers requires --symbolic or --symbolic-ply or --resume-dfa");
  }
  if (!options.compare_dfa.empty() &&
      (!options.symbolic && options.probe_dfa.empty())) {
    throw std::invalid_argument("--compare-dfa requires symbolic analysis or --probe-dfa");
  }
  if (options.symbolic || options.symbolic_verify) std::cout << std::unitbuf;
  std::unique_ptr<SymbolicDatabase> resumed;
  if (!options.resume_dfa.empty()) {
    resumed = std::make_unique<SymbolicDatabase>(options.resume_dfa, &options);
    options.width = resumed->width();
    options.height = resumed->height();
  }
  std::unique_ptr<SymbolicDatabase> symbolic_database;
  if (!options.probe_dfa.empty()) {
    symbolic_database = std::make_unique<SymbolicDatabase>(options.probe_dfa);
    options.width = symbolic_database->width();
    options.height = symbolic_database->height();
  }
  std::unique_ptr<WdlDatabase> database;
  if (!options.probe_db.empty()) {
    database = std::make_unique<WdlDatabase>(options.probe_db);
    options.width = database->width();
    options.height = database->height();
  }
  Twixt game(options.self_test ? 5 : options.width,
             options.self_test ? 5 : options.height);
  if (options.self_test) {
    game.self_test();
    Twixt rectangle(5, 6);
    if (rectangle.edge_count() != 54) {
      throw std::logic_error("5x6 edge count failed");
    }
    State sample;
    sample = rectangle.play(sample, rectangle.parse_cell("B1"));
    sample = rectangle.play(sample, rectangle.parse_cell("A2"));
    sample = rectangle.play(sample, rectangle.parse_cell("C3"));
    const Key packed = rectangle.canonical_key(sample, false);
    if (!(rectangle.canonical_key(rectangle.unpack_key(packed), false) == packed)) {
      throw std::logic_error("rectangular key round-trip failed");
    }
    if (!(rectangle.expand_compact_key(rectangle.compact_key(packed)) == packed)) {
      throw std::logic_error("compact key round-trip failed");
    }
    Twixt square7(7, 7);
    State square_state;
    for (int ply = 0; ply < 8; ++ply) {
      const std::vector<int> moves = square7.legal_moves(square_state);
      const CanonicalContext context =
          square7.prepare_canonical(square_state, true);
      for (int move : moves) {
        const State child = square7.play(square_state, move);
        if (!(square7.canonical_child_key(context, square_state, child, move) ==
              square7.canonical_key(child, true))) {
          throw std::logic_error("7x7 incremental canonicalization failed");
        }
      }
      square_state = square7.play(square_state, moves[(3 * ply + 1) % moves.size()]);
    }
    std::vector<DfaWord> dfa_test(4);
    dfa_test[1].set(1, 1);
    dfa_test[2].set(0, 1);
    dfa_test[3].set(0, 1);
    dfa_test[3].set(1, 1);
    const DfaStats dfa_stats = minimal_acyclic_dfa(dfa_test, 2);
    if (dfa_stats.states != 3 || dfa_stats.arcs != 4) {
      throw std::logic_error("minimal acyclic DFA test failed");
    }
    symbolic_move_self_test();
    std::cout << "self-test: ok\n";
    return 0;
  }

  if (options.estimate) {
    print_estimate(game);
    return 0;
  }

  State root;
  int replayed = 0;
  for (const std::string& token : split_moves(options.moves)) {
    if (game.has_won(root, other(root.turn))) {
      throw std::invalid_argument("move list continues after a winning connection");
    }
    const int cell = game.parse_cell(token);
    root = game.play(root, cell);
    ++replayed;
  }

  if (database) {
    run_database_probe(game, root, *database, options.quiet);
    return 0;
  }

  if (symbolic_database) {
    if (!options.compare_dfa.empty()) compare_symbolic(game, symbolic_database->solution(), options.compare_dfa);
    if (options.symbolic_verify) {
      verify_symbolic(symbolic_database->solution(), game,
                      symbolic_database->solution().root_state(), options, true);
    }
    probe_symbolic(game, root, symbolic_database->solution(), options.quiet);
    return 0;
  }

  if (options.symbolic) {
    return run_symbolic(game, resumed ? resumed->solution().root_state() : root,
                         options, resumed.get()) ? 0 : 3;
  }


  if (options.census_ply >= 0) {
    print_estimate(game);
    return run_census(game, root, options) ? 0 : 3;
  }

  if (options.strong) {
    print_estimate(game);
    return run_strong(game, root, options) ? 0 : 3;
  }

  if (!options.quiet) {
    std::cout << "Computer TwixT " << options.width << 'x' << options.height
              << ", " << replayed << " move(s) replayed\n";
    game.print_board(root, std::cout);
    std::cout << "Side to move: " << player_name(root.turn) << "\n"
              << "Knight-link edges: " << game.edge_count() << "\n"
              << "Solving exactly...\n";
  }

  Solver solver(game, options);
  const Outcome outcome = solver.solve(root);
  const std::string result = absolute_result(root, outcome);

  std::cout << "Result: " << (result == "draw" ? "Draw" : result == "white" ? "White win" : "Black win")
            << " (" << player_name(root.turn) << " to move: " << outcome_name(outcome) << ")\n";

  std::vector<std::pair<int, Outcome>> best;
  if (!game.has_won(root, other(root.turn))) {
    best = solver.root_moves(root, outcome, options.list_optimal);
  }
  if (!best.empty()) {
    std::cout << (options.list_optimal ? "Optimal moves:" : "Best move:");
    for (const auto& [move, ignored] : best) {
      (void)ignored;
      std::cout << ' ' << game.coordinate(move);
    }
    std::cout << '\n';
  }

  if (!options.quiet && options.pv_length > 0) {
    const std::vector<int> pv = solver.principal_variation(root, outcome, options.pv_length);
    if (!pv.empty()) {
      std::cout << "Principal variation:";
      for (int move : pv) std::cout << ' ' << game.coordinate(move);
      std::cout << '\n';
    }
  }

  const SearchStats& stats = solver.stats();
  std::cout << "Proof: " << stats.nodes << " nodes, " << solver.table_size() << " TT entries, "
            << stats.tt_hits << " TT hits, " << stats.draw_proofs << " draw proofs, "
            << std::fixed << std::setprecision(3) << solver.elapsed_seconds() << " s\n";
  if (stats.tt_dropped > 0) {
    std::cout << "Note: TT limit reached; " << stats.tt_dropped
              << " exact entries were not cached (result remains exact).\n";
  }

  if (options.expected.has_value() && *options.expected != result) {
    std::cerr << "expected " << *options.expected << " but obtained " << result << '\n';
    return 2;
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    return run(argc, argv);
  } catch (const twixt_dfa::Limit& limit) {
    std::cerr << "DFA resource cap: " << limit.what() << '\n';
    return 3;
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
  }
}
