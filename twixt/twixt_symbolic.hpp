// Included inside twixt.cpp's anonymous namespace, after the explicit solver.
// The symbolic solver uses the same geometry, but its move generation,
// terminal recognition, and minimax evaluation operate on whole DFA languages.

using DfaId = twixt_dfa::Id;
using SymbolWord = twixt_dfa::Word;

struct SymbolicLayer {
  DfaId reachable = 0;
  DfaId terminal_loss = 0;
  DfaId terminal_draw = 0;
  DfaId active = 0;
  DfaId win = 0;
  DfaId loss = 0;
  DfaId draw = 0;
};

struct FrontierKey {
  DfaId node = 0;
  std::array<std::uint8_t, kMaxCells> components{};
  bool operator==(const FrontierKey& rhs) const {
    return node == rhs.node && components == rhs.components;
  }
};

struct FrontierHash {
  std::size_t operator()(const FrontierKey& key) const noexcept {
    std::uint64_t h = twixt_dfa::hash64(key.node);
    for (unsigned i = 0; i < kMaxCells; i += 8) {
      std::uint64_t word = 0;
      for (unsigned j = 0; j < 8; ++j) {
        word |= static_cast<std::uint64_t>(key.components[i + j]) << (8 * j);
      }
      h = twixt_dfa::hash64(h ^ word);
    }
    return h;
  }
};

class SymbolicTwixt {
 public:
  SymbolicTwixt(const Twixt& game, const State& root, const Options& options)
      : game_(game), root_(root), options_(options),
        started_(std::chrono::steady_clock::now()),
        variables_(dfa_variables(game, true, false)),
        dfa_(alphabet(variables_), options.dfa_max_nodes, [this]() { check_time(); }) {
    cell_level_.fill(-1);
    edge_level_.fill(-1);
    retire_.resize(variables_.size());
    std::array<int, kMaxCells> last_use{};
    for (unsigned i = 0; i < variables_.size(); ++i) {
      const DfaVariable v = variables_[i];
      if (v.index < 0) turn_level_ = static_cast<int>(i);
      else if (v.link) {
        edge_level_[v.index] = static_cast<int>(i);
        last_use[game.edge(v.index).a] = static_cast<int>(i);
        last_use[game.edge(v.index).b] = static_cast<int>(i);
      } else {
        cell_level_[v.index] = static_cast<int>(i);
        last_use[v.index] = std::max(last_use[v.index], static_cast<int>(i));
      }
    }
    for (int cell = 0; cell < game.cell_count(); ++cell) {
      if (game.valid_cell(cell)) retire_[last_use[cell]].push_back(cell);
    }
    for (Player p : {Player::White, Player::Black}) {
      auto& future = future_start_[static_cast<int>(p)];
      future.resize(variables_.size() + 1);
      for (std::size_t i = variables_.size(); i-- > 0;) {
        const DfaVariable v = variables_[i];
        future[i] = future[i + 1] ||
                    (!v.link && v.index >= 0 && start_border(v.index, p));
      }
    }
  }

  twixt_dfa::Manager& manager() { return dfa_; }
  const twixt_dfa::Manager& manager() const { return dfa_; }
  std::vector<SymbolicLayer>& layers() { return layers_; }
  const std::vector<SymbolicLayer>& layers() const { return layers_; }
  const State& root_state() const { return root_; }
  void set_checkpoint_hook(std::function<void(unsigned, unsigned)> hook) {
    checkpoint_hook_ = std::move(hook);
  }
  double elapsed() const {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count();
  }

  SymbolWord encode(const State& state) const {
    SymbolWord result;
    result.reserve(variables_.size());
    for (const auto v : variables_) {
      if (v.index < 0) result.push_back(static_cast<std::uint8_t>(state.turn));
      else if (v.link) result.push_back(static_cast<std::uint8_t>(
          (state.links[v.index / 64] >> (v.index % 64)) & 1ULL));
      else result.push_back(static_cast<std::uint8_t>(
          ((state.white >> v.index) & 1ULL) != 0 ? 1 :
          ((state.black >> v.index) & 1ULL) != 0 ? 2 : 0));
    }
    return result;
  }

  Player turn(std::size_t ply) const { return ply % 2 == 0 ? root_.turn : other(root_.turn); }

  void check_time() const {
    if (strong_time_expired(options_, started_)) {
      throw twixt_dfa::Limit("symbolic wall-time cap reached");
    }
  }

  void collect(std::initializer_list<DfaId*> extra = {}, bool values_only = false) {
    std::vector<DfaId*> roots;
    for (auto& layer : layers_) {
      roots.push_back(&layer.reachable);
      roots.push_back(&layer.win);
      roots.push_back(&layer.loss);
      roots.push_back(&layer.draw);
      if (values_only) {
        layer.terminal_loss = layer.terminal_draw = layer.active = 0;
      } else {
        roots.push_back(&layer.terminal_loss);
        roots.push_back(&layer.terminal_draw);
        roots.push_back(&layer.active);
      }
    }
    roots.insert(roots.end(), extra.begin(), extra.end());
    dfa_.collect(roots);
    collected_size_ = dfa_.size();
  }

  void maybe_collect(std::initializer_list<DfaId*> extra) {
    if (dfa_.size() > collected_size_ + collected_size_ / 2 + 50'000 ||
        (options_.dfa_max_nodes != 0 && dfa_.size() > options_.dfa_max_nodes * 3 / 4)) {
      collect(extra);
    }
  }

  // Exact deterministic image of one placement.  Each condition depends only
  // on the other endpoint and crossing OLD links.  Incident links share the
  // new endpoint and cannot cross, so the conditional assignments commute.
  DfaId image(DfaId source, int move, Player mover) {
    if (!game_.legal_cell(move, mover) || source == 0) return 0;
    SymbolWord allowed = dfa_.unrestricted();
    allowed[cell_level_[move]] = 1;  // empty
    allowed[turn_level_] = static_cast<std::uint8_t>(1U << static_cast<int>(mover));
    for (int edge : game_.incident_edges(move)) allowed[edge_level_[edge]] = 1;
    DfaId current = dfa_.filter(source, allowed, cell_level_[move], 0, color(mover));
    const SymbolWord all = dfa_.unrestricted();
    for (int edge : game_.incident_edges(move)) {
      const SymbolWord condition = link_condition(move, edge, mover);
      current = dfa_.conditional_bit(current, condition, edge_level_[edge], false);
    }
    return dfa_.filter(current, all, turn_level_, static_cast<unsigned>(mover),
                       static_cast<unsigned>(other(mover)));
  }

  // Preimage of the SAME deterministic updates, reversed in composition
  // order.  In particular, an absent link is accepted only when the automatic
  // link condition is false; removing arbitrary child links is unsound.
  DfaId preimage(DfaId target, int move, Player mover) {
    if (!game_.legal_cell(move, mover) || target == 0) return 0;
    const SymbolWord all = dfa_.unrestricted();
    SymbolWord allowed = all;
    allowed[cell_level_[move]] = static_cast<std::uint8_t>(1U << color(mover));
    allowed[turn_level_] = static_cast<std::uint8_t>(1U << static_cast<int>(other(mover)));
    DfaId current = dfa_.filter(target, allowed, turn_level_,
                               static_cast<unsigned>(other(mover)), static_cast<unsigned>(mover));
    const auto& edges = game_.incident_edges(move);
    for (auto it = edges.rbegin(); it != edges.rend(); ++it) {
      const int edge = *it;
      const SymbolWord condition = link_condition(move, edge, mover);
      current = dfa_.conditional_bit(current, condition, edge_level_[edge], true);
    }
    return dfa_.filter(current, all, cell_level_[move], color(mover), 0);
  }

  // Recognize a connection while scanning the spatial variable order.  The
  // state tracks only components touching an unfinished part of the board.
  // Low six bits are canonical component labels, high bits mark the borders.
  DfaId won(DfaId source, Player player) {
    if (source == 0) return 0;
    std::unordered_map<FrontierKey, DfaId, FrontierHash> memo;
    std::uint64_t visits = 0;
    std::function<DfaId(DfaId, const std::array<std::uint8_t, kMaxCells>&)> visit;
    visit = [&](DfaId id, const auto& components) -> DfaId {
      if (id < 2) return 0;
      if (((++visits) & 0xfffULL) == 0) check_time();
      FrontierKey key{id, components};
      const auto found = memo.find(key);
      if (found != memo.end()) return found->second;
      const twixt_dfa::Node node = dfa_.node(id);
      const DfaVariable variable = variables_[node.level];
      twixt_dfa::Children children{};
      for (unsigned symbol = 0; symbol < dfa_.alphabet()[node.level]; ++symbol) {
        if (node.next[symbol] == 0) continue;
        auto next = components;
        bool connected = false;
        if (!variable.link && variable.index >= 0 && symbol == color(player)) {
          next[variable.index] = static_cast<std::uint8_t>(
              63 | (start_border(variable.index, player) ? 64 : 0) |
              (target_border(variable.index, player) ? 128 : 0));
        } else if (variable.link && symbol == 1) {
          const Edge& edge = game_.edge(variable.index);
          const unsigned a = next[edge.a], b = next[edge.b];
          if (a != 0 && b != 0) {
            const unsigned merged = (a & 63) | ((a | b) & 192);
            connected = (merged & 192) == 192;
            for (auto& label : next) {
              if (label != 0 && ((label & 63) == (a & 63) ||
                                 (label & 63) == (b & 63))) {
                label = static_cast<std::uint8_t>(merged);
              }
            }
          }
        }
        if (connected) {
          children[symbol] = node.next[symbol];
          continue;
        }
        for (int cell : retire_[node.level]) next[cell] = 0;
        bool has_start = false;
        std::array<std::uint8_t, 64> normalized{};
        std::uint8_t labels = 0;
        for (auto& label : next) {
          if (label == 0) continue;
          has_start = has_start || (label & 64) != 0;
          const unsigned old_label = label & 63;
          if (normalized[old_label] == 0) normalized[old_label] = ++labels;
          label = static_cast<std::uint8_t>(normalized[old_label] | (label & 192));
        }
        if (has_start || future_start_[static_cast<int>(player)][node.level + 1]) {
          children[symbol] = visit(node.next[symbol], next);
        }
      }
      const DfaId result = dfa_.intern(node.level, children);
      memo.emplace(std::move(key), result);
      return result;
    };
    return visit(source, {});
  }

  void add_layer(DfaId reachable) {
    const std::size_t ply = layers_.size();
    layers_.push_back(SymbolicLayer{});
    auto& layer = layers_.back();
    layer.reachable = reachable;
    layer.terminal_loss = won(reachable, other(turn(ply)));
    SymbolWord no_moves = dfa_.unrestricted();
    for (int cell = 0; cell < game_.cell_count(); ++cell) {
      if (game_.legal_cell(cell, turn(ply))) no_moves[cell_level_[cell]] = 6;
    }
    const DfaId exhausted = dfa_.filter(reachable, no_moves);
    layer.terminal_draw = dfa_.difference(exhausted, layer.terminal_loss);
    layer.active = dfa_.difference(reachable, dfa_.unite(layer.terminal_loss, layer.terminal_draw));
  }

  bool forward() {
    if (layers_.empty()) add_layer(dfa_.singleton(encode(root_)));
    std::cout << "Symbolic forward generation (no symmetry; dead-draw descendants retained)\n"
              << "ply,positions,dfa_states,active_positions,pool_nodes,seconds\n";
    for (std::size_t ply = layers_.size() - 1;; ++ply) {
      collect();
      if (checkpoint_hook_) checkpoint_hook_(0, 0);
      const auto& layer = layers_[ply];
      std::cout << ply << ',' << dfa_.count(layer.reachable) << ','
                << dfa_.reachable_nodes(layer.reachable) << ',' << dfa_.count(layer.active)
                << ',' << dfa_.size() << ',' << std::fixed << std::setprecision(3)
                << elapsed() << std::endl;
      if (layer.active == 0) return true;
      if (options_.symbolic_ply >= 0 && ply >= static_cast<std::size_t>(options_.symbolic_ply)) {
        return false;
      }
      DfaId next = 0;
      for (int cell = 0; cell < game_.cell_count(); ++cell) {
        if (!game_.legal_cell(cell, turn(ply))) continue;
        // collect changes all root IDs; access the source again each time.
        const DfaId child = image(layers_[ply].active, cell, turn(ply));
        next = dfa_.unite(next, child);
        maybe_collect({&next});
      }
      if (next == 0) throw std::logic_error("active language has no successors");
      add_layer(next);
    }
  }

  void retrograde(std::size_t remaining = std::numeric_limits<std::size_t>::max()) {
    std::cout << "Symbolic retrograde\nply,win,draw,loss,wdl_dfa_states,pool_nodes,seconds\n";
    if (remaining == std::numeric_limits<std::size_t>::max()) remaining = layers_.size();
    // Solved W/L/D languages partition reachability.  Keeping their separate
    // R and active/terminal languages would retain millions of redundant
    // nodes.  This also upgrades older checkpoints that retained those roots.
    for (std::size_t ply = remaining; ply < layers_.size(); ++ply) {
      layers_[ply].reachable = layers_[ply].active = 0;
      layers_[ply].terminal_loss = layers_[ply].terminal_draw = 0;
    }
    for (std::size_t ply = remaining; ply-- > 0;) {
      layers_[ply].loss = layers_[ply].terminal_loss;
      layers_[ply].draw = layers_[ply].terminal_draw;
      if (layers_[ply].active != 0) {
        if (ply + 1 == layers_.size()) throw std::logic_error("incomplete final symbolic layer");
        DfaId to_loss = 0, to_nonwin = 0;
        DfaId nonwin = dfa_.unite(layers_[ply + 1].loss, layers_[ply + 1].draw);
        for (int cell = 0; cell < game_.cell_count(); ++cell) {
          if (!game_.legal_cell(cell, turn(ply))) continue;
          DfaId parents = preimage(layers_[ply + 1].loss, cell, turn(ply));
          parents = dfa_.intersect(parents, layers_[ply].active);
          to_loss = dfa_.unite(to_loss, parents);
          parents = preimage(nonwin, cell, turn(ply));
          parents = dfa_.intersect(parents, layers_[ply].active);
          to_nonwin = dfa_.unite(to_nonwin, parents);
          maybe_collect({&to_loss, &to_nonwin, &nonwin});
        }
        layers_[ply].win = to_loss;
        layers_[ply].loss = dfa_.unite(layers_[ply].loss,
                                      dfa_.difference(layers_[ply].active, to_nonwin));
        layers_[ply].draw = dfa_.difference(layers_[ply].reachable,
                               dfa_.unite(layers_[ply].win, layers_[ply].loss));
      }
      layers_[ply].reachable = layers_[ply].active = 0;
      layers_[ply].terminal_loss = layers_[ply].terminal_draw = 0;
      collect();
      if (checkpoint_hook_) checkpoint_hook_(1, static_cast<unsigned>(ply));
      const auto& layer = layers_[ply];
      std::cout << ply << ',' << dfa_.count(layer.win) << ',' << dfa_.count(layer.draw)
                << ',' << dfa_.count(layer.loss) << ','
                << dfa_.reachable_nodes(layer.win) + dfa_.reachable_nodes(layer.draw) +
                   dfa_.reachable_nodes(layer.loss)
                << ',' << dfa_.size() << ',' << std::fixed << std::setprecision(3)
                << elapsed() << std::endl;
    }
  }

  Outcome value(const State& state, std::size_t ply) const {
    if (ply >= layers_.size()) throw std::invalid_argument("position outside DFA layers");
    const SymbolWord word = encode(state);
    const auto& layer = layers_[ply];
    if (layer.reachable != 0 && !dfa_.contains(layer.reachable, word)) {
      throw std::invalid_argument("position is absent from the DFA solution");
    }
    const bool w = dfa_.contains(layer.win, word), l = dfa_.contains(layer.loss, word);
    const bool d = dfa_.contains(layer.draw, word);
    if (!w && !l && !d) throw std::invalid_argument("position is absent from the DFA solution");
    if (static_cast<int>(w) + l + d != 1) throw std::logic_error("invalid DFA WDL partition");
    return w ? Outcome::Win : l ? Outcome::Loss : Outcome::Draw;
  }

 private:
  const Twixt& game_;
  State root_;
  Options options_;
  std::chrono::steady_clock::time_point started_;
  std::vector<DfaVariable> variables_;
  twixt_dfa::Manager dfa_;
  std::vector<SymbolicLayer> layers_;
  std::array<int, kMaxCells> cell_level_{};
  std::array<int, kMaxEdges> edge_level_{};
  int turn_level_ = -1;
  std::vector<std::vector<int>> retire_;
  std::array<std::vector<bool>, 2> future_start_;
  std::size_t collected_size_ = 0;
  std::function<void(unsigned, unsigned)> checkpoint_hook_;

  static std::vector<std::uint8_t> alphabet(const std::vector<DfaVariable>& variables) {
    std::vector<std::uint8_t> result;
    for (auto v : variables) result.push_back(v.link || v.index < 0 ? 2 : 3);
    return result;
  }
  static unsigned color(Player player) { return player == Player::White ? 1 : 2; }
  bool start_border(int cell, Player player) const {
    return player == Player::White ? cell / game_.width() == 0 : cell % game_.width() == 0;
  }
  bool target_border(int cell, Player player) const {
    return player == Player::White ? cell / game_.width() == game_.height() - 1
                                  : cell % game_.width() == game_.width() - 1;
  }
  SymbolWord link_condition(int move, int edge, Player mover) const {
    SymbolWord condition = dfa_.unrestricted();
    const Edge& e = game_.edge(edge);
    const int neighbor = e.a == move ? e.b : e.a;
    condition[cell_level_[neighbor]] = static_cast<std::uint8_t>(1U << color(mover));
    for (int crossing = 0; crossing < game_.edge_count(); ++crossing) {
      if (game_.edges_cross(edge, crossing)) condition[edge_level_[crossing]] = 1;
    }
    return condition;
  }
};

bool verify_symbolic(SymbolicTwixt& symbolic, const Twixt& game,
                     const State& root, Options options, bool completed) {
  // Independent explicit enumeration.  Check exact set equality, both border
  // connectivity languages, and (for a complete run) every single WDL value.
  options.symmetry = false;
  options.draw_pruning = false;
  options.census_seconds = 0;
  if (completed) {
    const auto reference = build_strong_solution(game, root, options);
    if (!reference) throw std::runtime_error("explicit reference solve did not finish");
    if (reference->layers.size() != symbolic.layers().size()) {
      throw std::logic_error("symbolic/explicit layer count mismatch");
    }
    for (std::size_t ply = 0; ply < reference->layers.size(); ++ply) {
      const auto& layer = symbolic.layers()[ply];
      const DfaId reachable = layer.reachable != 0 ? layer.reachable :
          symbolic.manager().unite(symbolic.manager().unite(layer.win, layer.loss), layer.draw);
      if (symbolic.manager().count(reachable) != reference->layers[ply].size()) {
        throw std::logic_error("symbolic/explicit position count mismatch");
      }
      const DfaId white = symbolic.won(reachable, Player::White);
      const DfaId black = symbolic.won(reachable, Player::Black);
      for (std::size_t i = 0; i < reference->layers[ply].size(); ++i) {
        const State state = game.unpack_key(reference->layers[ply][i]);
        const SymbolWord word = symbolic.encode(state);
        if (symbolic.manager().contains(white, word) != game.has_won(state, Player::White) ||
            symbolic.manager().contains(black, word) != game.has_won(state, Player::Black) ||
            static_cast<std::int8_t>(symbolic.value(state, ply)) != reference->values[ply][i]) {
          throw std::logic_error("symbolic/explicit connectivity or WDL mismatch");
        }
      }
      symbolic.collect();
      std::cout << "  verified ply " << ply << ": " << reference->layers[ply].size()
                << " positions and values\n";
    }
    std::cout << "Verified all " << reference->total_states
              << " reachable states and both connectivity predicates.\n"
              << "Verified all " << reference->total_states
              << " WDL values against explicit retrograde.\n";
    return true;
  }
  std::uint64_t checked = 0;
  std::vector<Key> explicit_layer{game.canonical_key(root, false)};
  for (std::size_t ply = 0; ply < symbolic.layers().size(); ++ply) {
    if (symbolic.manager().count(symbolic.layers()[ply].reachable) != explicit_layer.size()) {
      throw std::logic_error("symbolic/explicit layer count mismatch at ply " + std::to_string(ply));
    }
    DfaId white_win = symbolic.won(symbolic.layers()[ply].reachable, Player::White);
    DfaId black_win = symbolic.won(symbolic.layers()[ply].reachable, Player::Black);
    std::unordered_set<Key, KeyHash> next;
    for (const Key& key : explicit_layer) {
      const State state = game.unpack_key(key);
      const SymbolWord word = symbolic.encode(state);
      if (!symbolic.manager().contains(symbolic.layers()[ply].reachable, word) ||
          symbolic.manager().contains(white_win, word) != game.has_won(state, Player::White) ||
          symbolic.manager().contains(black_win, word) != game.has_won(state, Player::Black)) {
        throw std::logic_error("symbolic reachability/connectivity mismatch");
      }
      ++checked;
      if (ply + 1 == symbolic.layers().size() || terminal_outcome(game, state, false).has_value()) continue;
      for (int move : game.legal_moves(state)) {
        next.insert(game.canonical_key(game.play(state, move), false));
        if (options.census_max_states > 0 && next.size() > options.census_max_states) {
          throw twixt_dfa::Limit("explicit verification state cap reached");
        }
      }
    }
    explicit_layer.assign(next.begin(), next.end());
    symbolic.collect();
  }
  std::cout << "Verified all " << checked << " reachable states and both connectivity predicates.\n";
  return true;
}

std::uint64_t save_symbolic_archive(const std::string& path, const Twixt& game,
                           SymbolicTwixt& solution, bool checkpoint,
                           unsigned phase = 2, unsigned remaining = 0) {
  // Atomic replacement leaves the previous complete layer usable if writing
  // is interrupted.  The adjacent .partial file is never treated as a result.
  const std::string temporary = path + ".partial";
  std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
  if (!out) throw std::runtime_error("cannot open DFA solution output: " + path);
  out.write(checkpoint ? "TWXCKP01" : "TWXDFA01", 8);
  write_u32(out, 2);  // sparse arcs; version 1 stored level + three child IDs
  write_u32(out, static_cast<std::uint32_t>(game.width()));
  write_u32(out, static_cast<std::uint32_t>(game.height()));
  write_u32(out, 1);  // spatial variable order
  write_u32(out, solution.manager().length());
  write_u64(out, solution.manager().size());
  write_u32(out, static_cast<std::uint32_t>(solution.layers().size()));
  const Key root_key = game.canonical_key(solution.root_state(), false);
  for (auto word : root_key.words) write_u64(out, word);
  if (checkpoint) {
    write_u32(out, phase);
    write_u32(out, remaining);
  }
  for (const auto& layer : solution.layers()) {
    write_u32(out, layer.reachable);
    write_u32(out, layer.win);
    write_u32(out, layer.loss);
    write_u32(out, layer.draw);
    if (checkpoint) {
      write_u32(out, layer.terminal_loss);
      write_u32(out, layer.terminal_draw);
      write_u32(out, layer.active);
    }
  }
  std::uint64_t node_bytes = 0;
  for (std::size_t id = 2; id < solution.manager().size(); ++id) {
    const auto& node = solution.manager().nodes()[id];
    unsigned mask = 0;
    for (unsigned c = 0; c < 3; ++c) if (node.next[c] != 0) mask |= 1U << c;
    out.put(static_cast<char>(mask));
    ++node_bytes;
    for (DfaId child : node.next) {
      if (child != 0) { write_u32(out, child); node_bytes += 4; }
    }
  }
  out.flush();
  if (!out) throw std::runtime_error("failed to write DFA solution: " + path);
  out.close();
  if (!out || std::rename(temporary.c_str(), path.c_str()) != 0) {
    throw std::runtime_error("cannot finish DFA archive: " + path);
  }
  return node_bytes;
}

void save_symbolic(const std::string& path, const Twixt& game, SymbolicTwixt& solution) {
  solution.collect({}, true);
  const std::uint64_t node_bytes = save_symbolic_archive(path, game, solution, false);
  std::cout << "DFA solution written to " << path << " (" << solution.manager().size()
            << " shared nodes, " << node_bytes << " sparse node bytes).\n";
}

class SymbolicDatabase {
 public:
  explicit SymbolicDatabase(const std::string& path, const Options* run_options = nullptr) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open DFA solution: " + path);
    char magic[8]{};
    in.read(magic, 8);
    checkpoint_ = std::string(magic, 8) == "TWXCKP01";
    const unsigned version = read_u32(in);
    if ((!checkpoint_ && std::string(magic, 8) != "TWXDFA01") || (version != 1 && version != 2)) {
      throw std::runtime_error("not a supported TWXDFA01 solution");
    }
    if (checkpoint_ && run_options == nullptr) {
      throw std::invalid_argument("checkpoint requires --resume-dfa, not --probe-dfa");
    }
    width_ = static_cast<int>(read_u32(in));
    height_ = static_cast<int>(read_u32(in));
    if (read_u32(in) != 1) throw std::runtime_error("unknown DFA variable order");
    const unsigned length = read_u32(in);
    const std::uint64_t node_count = read_u64(in);
    const unsigned layer_count = read_u32(in);
    game_ = std::make_unique<Twixt>(width_, height_);
    Options options;
    options.census_seconds = 0;
    options.dfa_max_nodes = 0;
    if (run_options) options = *run_options;
    Key root_key;
    for (auto& word : root_key.words) word = read_u64(in);
    if (checkpoint_) {
      phase_ = read_u32(in);
      remaining_ = read_u32(in);
    }
    solution_ = std::make_unique<SymbolicTwixt>(*game_, game_->unpack_key(root_key), options);
    if (length != solution_->manager().length() || node_count < 2 ||
        node_count >= std::numeric_limits<DfaId>::max() ||
        layer_count == 0 || layer_count > kMaxCells + 1 || phase_ > 2 ||
        remaining_ > layer_count) {
      throw std::runtime_error("invalid DFA solution dimensions");
    }
    const auto records_begin = in.tellg();
    in.seekg(0, std::ios::end);
    const std::uint64_t file_size = static_cast<std::uint64_t>(in.tellg());
    const std::uint64_t data_begin = static_cast<std::uint64_t>(records_begin) +
                                    (checkpoint_ ? 28ULL : 16ULL) * layer_count;
    const std::uint64_t minimum = data_begin + (version == 1 ? 16ULL : 5ULL) * (node_count - 2);
    const std::uint64_t maximum = data_begin + (version == 1 ? 16ULL : 13ULL) * (node_count - 2);
    if (file_size < minimum || file_size > maximum) {
      throw std::runtime_error("DFA solution file size mismatch");
    }
    in.seekg(records_begin);
    solution_->layers().resize(layer_count);
    for (auto& layer : solution_->layers()) {
      layer.reachable = read_u32(in);
      layer.win = read_u32(in);
      layer.loss = read_u32(in);
      layer.draw = read_u32(in);
      if (checkpoint_) {
        layer.terminal_loss = read_u32(in);
        layer.terminal_draw = read_u32(in);
        layer.active = read_u32(in);
      }
      for (DfaId root : {layer.reachable, layer.win, layer.loss, layer.draw,
                         layer.terminal_loss, layer.terminal_draw, layer.active}) {
        if (root >= node_count) throw std::runtime_error("invalid DFA root ID");
      }
    }
    for (std::uint64_t id = 2; id < node_count; ++id) {
      unsigned level = 0;
      if (version == 1) level = read_u32(in);
      const unsigned mask = version == 1 ? 7 : static_cast<unsigned>(in.get());
      if (mask == 0 || mask > 7) throw std::runtime_error("invalid sparse DFA arc mask");
      twixt_dfa::Children children{};
      for (unsigned c = 0; c < 3; ++c) {
        if ((mask & (1U << c)) == 0) continue;
        const DfaId child = children[c] = read_u32(in);
        if (child >= id || (version == 2 && child == 0)) {
          throw std::runtime_error("invalid DFA topological order");
        }
        if (version == 2 && child != 0) {
          const unsigned child_level = solution_->manager().node(child).level;
          if (child_level == 0) throw std::runtime_error("invalid DFA transition level");
          level = child_level - 1;
        }
      }
      if (solution_->manager().intern(level, children) != id) {
        throw std::runtime_error("nonminimal or duplicate DFA node");
      }
    }
    if (static_cast<std::uint64_t>(in.tellg()) != file_size) {
      throw std::runtime_error("DFA solution has trailing or missing data");
    }
    for (const auto& layer : solution_->layers()) {
      for (DfaId root : {layer.reachable, layer.win, layer.loss, layer.draw,
                         layer.terminal_loss, layer.terminal_draw, layer.active}) {
        if (root != 0 && (root == 1 || solution_->manager().node(root).level != 0)) {
          throw std::runtime_error("DFA language root is not at level zero");
        }
      }
    }
  }
  int width() const { return width_; }
  int height() const { return height_; }
  SymbolicTwixt& solution() { return *solution_; }
  unsigned phase() const { return phase_; }
  unsigned remaining() const { return remaining_; }
 private:
  int width_ = 0, height_ = 0;
  bool checkpoint_ = false;
  unsigned phase_ = 2, remaining_ = 0;
  std::unique_ptr<Twixt> game_;
  std::unique_ptr<SymbolicTwixt> solution_;
};

void probe_symbolic(const Twixt& game, const State& state,
                    SymbolicTwixt& solution, bool quiet) {
  const int ply = peg_count(state) - peg_count(solution.root_state());
  if (ply < 0) throw std::invalid_argument("position precedes DFA solution root");
  const Outcome value = solution.value(state, static_cast<std::size_t>(ply));
  if (!quiet) game.print_board(state, std::cout);
  std::cout << "DFA WDL (" << player_name(state.turn) << " to move): " << outcome_name(value) << '\n';
  if (terminal_outcome(game, state, false).has_value()) return;
  std::cout << "Moves:";
  for (int move : game.legal_moves(state)) {
    const Outcome v = negate(solution.value(game.play(state, move), static_cast<std::size_t>(ply + 1)));
    std::cout << ' ' << game.coordinate(move) << '=' << (v == Outcome::Win ? 'W' : v == Outcome::Loss ? 'L' : 'D');
    if (v == value) std::cout << '*';
  }
  std::cout << '\n';
}

bool run_symbolic(const Twixt& game, const State& root, const Options& options,
                   SymbolicDatabase* resumed = nullptr) {
  try {
    std::unique_ptr<SymbolicTwixt> fresh;
    if (!resumed) fresh = std::make_unique<SymbolicTwixt>(game, root, options);
    SymbolicTwixt& solution = resumed ? resumed->solution() : *fresh;
    if (!options.dfa_checkpoint.empty()) {
      solution.set_checkpoint_hook([&](unsigned phase, unsigned remaining) {
        save_symbolic_archive(options.dfa_checkpoint, game, solution, true, phase, remaining);
      });
    }
    const bool completed = resumed && resumed->phase() > 0 ? true : solution.forward();
    if (completed && (!resumed || resumed->phase() < 2)) {
      solution.retrograde(resumed && resumed->phase() == 1 ? resumed->remaining() :
                          std::numeric_limits<std::size_t>::max());
    }
    if (options.symbolic_verify) verify_symbolic(solution, game, root, options, completed);
    if (!completed) {
      std::cout << "Symbolic census completed through ply " << options.symbolic_ply
                << "; no WDL solution claimed.\n";
      return true;
    }
    const Outcome value = solution.value(root, 0);
    const std::string result = absolute_result(root, value);
    std::cout << "Symbolic strong result: " << result << " (" << solution.layers().size()
              << " layers, " << std::fixed << std::setprecision(3) << solution.elapsed() << " s).\n";
    if (!options.dfa_out.empty()) save_symbolic(options.dfa_out, game, solution);
    if (!options.dfa_checkpoint.empty()) {
      save_symbolic_archive(options.dfa_checkpoint, game, solution, true, 2, 0);
    }
    if (options.expected && *options.expected != result) {
      throw std::runtime_error("symbolic result differs from --expect");
    }
    return true;
  } catch (const twixt_dfa::Limit& limit) {
    std::cout << "Symbolic computation stopped: " << limit.what()
              << ". No complete solution file written.\n";
    return false;
  }
}

void symbolic_move_self_test() {
  Twixt game(5, 6);
  Options options;
  options.census_seconds = 0;
  SymbolicTwixt symbolic(game, State{}, options);
  std::uint64_t random = 0x123456789abcdefULL;
  for (int trial = 0; trial < 16; ++trial) {
    State state;
    for (int ply = 0; ply < 18; ++ply) {
      if (terminal_outcome(game, state, false).has_value()) break;
      const auto moves = game.legal_moves(state);
      random = mix64(random);
      const int move = moves[random % moves.size()];
      const State child = game.play(state, move);
      DfaId source = symbolic.manager().singleton(symbolic.encode(state));
      DfaId expected = symbolic.manager().singleton(symbolic.encode(child));
      if (symbolic.image(source, move, state.turn) != expected) {
        throw std::logic_error("symbolic placement differs from explicit play");
      }
      DfaId parents = symbolic.preimage(expected, move, state.turn);
      if (!symbolic.manager().contains(parents, symbolic.encode(state))) {
        throw std::logic_error("symbolic preimage omits a true parent");
      }
      // Toggling ANY incident link gives a false child, whether the link was
      // mandatory, blocked by a crossing, or has no friendly other endpoint.
      for (int edge : game.incident_edges(move)) {
        State corrupted = child;
        corrupted.links[edge / 64] ^= 1ULL << (edge % 64);
        DfaId bad = symbolic.manager().singleton(symbolic.encode(corrupted));
        parents = symbolic.preimage(bad, move, state.turn);
        if (symbolic.manager().contains(parents, symbolic.encode(state))) {
          throw std::logic_error("symbolic preimage accepts an invalid automatic link");
        }
      }
      symbolic.collect();
      state = child;
    }
  }
}
