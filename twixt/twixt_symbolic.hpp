// Included inside twixt.cpp's anonymous namespace, after the explicit solver.
// The symbolic solver uses the same geometry, but its move generation,
// terminal recognition, and minimax evaluation operate on whole DFA languages.

using DfaId = twixt_dfa::Id;
using SymbolWord = twixt_dfa::Word;

// Opt-in memory profiling (TWIXT_PROFILE=path). Disabled, it costs one branch.
class Profile {
 public:
  static Profile& get() { static Profile profile; return profile; }
  bool on() const { return out_ != nullptr; }
  bool reach() const { return reach_; }
  bool mallinfo() const { return mallinfo_; }
  bool scan() const { return scan_; }
  double seconds() const {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
  }
  void line(const char* format, ...) __attribute__((format(printf, 2, 3))) {
    if (!out_) return;
    std::lock_guard<std::mutex> guard(mutex_);
    std::fprintf(out_, "%.3f,", seconds());
    va_list args;
    va_start(args, format);
    std::vfprintf(out_, format, args);
    va_end(args);
    std::fputc('\n', out_);
    std::fflush(out_);
  }
 private:
  static bool flag(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && *value != '\0' && *value != '0';
  }
  Profile() {
    if (const char* path = std::getenv("TWIXT_PROFILE")) out_ = std::fopen(path, "w");
    reach_ = flag("TWIXT_PROFILE_REACH");
    mallinfo_ = flag("TWIXT_PROFILE_MALLINFO");
    scan_ = flag("TWIXT_PROFILE_SCAN");
  }
  std::FILE* out_ = nullptr;
  bool reach_ = false, mallinfo_ = false, scan_ = false;
  std::mutex mutex_;
  std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
};
inline thread_local int profile_ply = -1;
inline thread_local long profile_job = -1;
// Memo size of this thread's last fused image/preimage (diagnostics only;
// per thread because concurrent workers run fused operations).
inline thread_local std::size_t profile_fused_memo = 0;
inline thread_local std::size_t profile_fused_multi = 0;

inline std::pair<std::uint64_t, std::uint64_t> self_vsz_rss() {
  std::ifstream in("/proc/self/statm");
  std::uint64_t virtual_pages = 0, resident_pages = 0;
  in >> virtual_pages >> resident_pages;
  return {virtual_pages * 4096ULL, resident_pages * 4096ULL};
}

// ---------------------------------------------------------------------------
// Per-layer storage.  Each finished layer is written to its own file with
// compact local node numbering, so only the layers a step needs stay live:
// forward generation needs active(k); retrograde ply k needs layer k's
// forward languages and layer k+1's W/L/D.  A small text manifest, replaced
// atomically, is the commit point; files it does not list are ignored.
//
// Layer file (little endian):
//   "TWXLAY01", u32 version=1, u32 width, u32 height, u32 order=1,
//   u32 length, u32 ply, u32 kind (0 forward, 1 result), u32 root_count,
//   u32 id_bytes (4 or 5), u64 node_count, u64 roots[root_count] (local IDs),
//   node_count records in topological order (children first): u8 arc mask,
//   then one local child ID per set bit (local IDs: 0 reject, 1 accept,
//   node i is 2+i), and a trailing u64 checksum of all preceding bytes.
// A node's level is implied by any child (child level - 1, or length - 1
// for the accepting terminal), exactly as in version 2 solution archives.

class Checksum {
 public:
  void update(const char* data, std::size_t n) {
    total_ += n;
    if (fill_ == 0) {  // whole little-endian words, as the byte loop below assembles them
      for (; n >= 8; data += 8, n -= 8) {
        std::uint64_t w = 0;
        for (int i = 7; i >= 0; --i) w = (w << 8) | static_cast<unsigned char>(data[i]);
        state_ = twixt_dfa::hash64(state_ ^ w) + 0x9e3779b97f4a7c15ULL;
      }
    }
    while (n > 0) {
      const std::size_t take = std::min<std::size_t>(n, 8 - fill_);
      std::memcpy(word_ + fill_, data, take);
      fill_ += take; data += take; n -= take;
      if (fill_ == 8) {
        std::uint64_t w = 0;
        for (int i = 7; i >= 0; --i) w = (w << 8) | static_cast<unsigned char>(word_[i]);
        state_ = twixt_dfa::hash64(state_ ^ w) + 0x9e3779b97f4a7c15ULL;
        fill_ = 0;
      }
    }
  }
  std::uint64_t value() const {
    std::uint64_t w = 0;
    for (int i = static_cast<int>(fill_) - 1; i >= 0; --i) w = (w << 8) | static_cast<unsigned char>(word_[i]);
    return twixt_dfa::hash64(twixt_dfa::hash64(state_ ^ w) ^ total_);
  }
 private:
  std::uint64_t state_ = 0x243f6a8885a308d3ULL, total_ = 0;
  char word_[8]{};
  std::size_t fill_ = 0;
};

class BufferedOut {
 public:
  explicit BufferedOut(const std::string& path) : out_(path, std::ios::binary | std::ios::trunc) {
    if (!out_) throw std::runtime_error("cannot create layer file: " + path);
    buffer_.reserve(kSize);
  }
  void put(std::uint64_t value, unsigned bytes) {
    for (unsigned b = 0; b < bytes; ++b) buffer_.push_back(static_cast<char>((value >> (8 * b)) & 0xffU));
    if (buffer_.size() >= kSize) flush();
  }
  void put_raw(const char* data, std::size_t n) { for (std::size_t i = 0; i < n; ++i) put(static_cast<unsigned char>(data[i]), 1); }
  void put_block(const char* data, std::size_t n) {
    flush();
    sum_.update(data, n);
    out_.write(data, static_cast<std::streamsize>(n));
    bytes_ += n;
    if (!out_) throw std::runtime_error("failed writing layer file");
  }
  void flush() {
    sum_.update(buffer_.data(), buffer_.size());
    out_.write(buffer_.data(), static_cast<std::streamsize>(buffer_.size()));
    bytes_ += buffer_.size();
    buffer_.clear();
    if (!out_) throw std::runtime_error("failed writing layer file");
  }
  // Append the checksum of everything written so far, then close.
  std::uint64_t finish() {
    flush();
    const std::uint64_t checksum = sum_.value();
    for (unsigned b = 0; b < 8; ++b) buffer_.push_back(static_cast<char>((checksum >> (8 * b)) & 0xffU));
    out_.write(buffer_.data(), 8);
    bytes_ += 8;
    out_.close();
    if (!out_) throw std::runtime_error("failed closing layer file");
    return checksum;
  }
  std::uint64_t bytes() const { return bytes_; }
  std::uint64_t position() const { return bytes_ + buffer_.size(); }
 private:
  static constexpr std::size_t kSize = 1 << 24;
  std::ofstream out_;
  std::vector<char> buffer_;
  Checksum sum_;
  std::uint64_t bytes_ = 0;
};

class BufferedIn {
 public:
  explicit BufferedIn(const std::string& path) : in_(path, std::ios::binary) {
    if (!in_) throw std::runtime_error("cannot open layer file: " + path);
    in_.seekg(0, std::ios::end);
    size_ = static_cast<std::uint64_t>(in_.tellg());
    in_.seekg(0);
    if (size_ < 8) throw std::runtime_error("truncated layer file: " + path);
    content_ = size_ - 8;
  }
  std::uint64_t get(unsigned bytes) {
    std::uint64_t value = 0;
    for (unsigned b = 0; b < bytes; ++b) value |= static_cast<std::uint64_t>(byte()) << (8 * b);
    return value;
  }
  // Verify the checksum once every content byte has been consumed.
  void finish() {
    if (consumed_ != content_) throw std::runtime_error("layer file has trailing or missing data");
    sum_.update(buffer_.data(), pos_);
    unsigned char tail[8];
    in_.seekg(static_cast<std::streamoff>(content_));
    if (!in_.read(reinterpret_cast<char*>(tail), 8)) throw std::runtime_error("truncated layer checksum");
    std::uint64_t stored = 0;
    for (int b = 7; b >= 0; --b) stored = (stored << 8) | tail[b];
    if (stored != sum_.value()) throw std::runtime_error("layer file checksum mismatch");
  }
 private:
  unsigned char byte() {
    if (pos_ == buffer_.size()) refill();
    ++consumed_;
    return static_cast<unsigned char>(buffer_[pos_++]);
  }
  void refill() {
    sum_.update(buffer_.data(), buffer_.size());
    const std::uint64_t left = content_ - read_;
    if (left == 0) throw std::runtime_error("truncated layer file");
    buffer_.resize(static_cast<std::size_t>(std::min<std::uint64_t>(left, 1 << 24)));
    if (!in_.read(buffer_.data(), static_cast<std::streamsize>(buffer_.size()))) {
      throw std::runtime_error("failed reading layer file");
    }
    read_ += buffer_.size();
    pos_ = 0;
  }
  std::ifstream in_;
  std::vector<char> buffer_;
  std::size_t pos_ = 0;
  std::uint64_t size_ = 0, content_ = 0, read_ = 0, consumed_ = 0;
  Checksum sum_;
};

// Run body(item) for every item in [0, items) on up to `threads` threads.  The
// first exception stops the remaining items and is rethrown.
inline void parallel_items(unsigned threads, std::size_t items, const std::function<void(std::size_t)>& body) {
  std::atomic<std::size_t> next{0};
  std::atomic<bool> failed{false};
  std::exception_ptr error;
  std::mutex error_mutex;
  auto run = [&]() {
    try {
      for (std::size_t item; !failed.load(std::memory_order_relaxed) && (item = next.fetch_add(1)) < items;) body(item);
    } catch (...) {
      std::lock_guard<std::mutex> guard(error_mutex);
      if (!error) error = std::current_exception();
      failed.store(true);
    }
  };
  std::vector<std::thread> pool;
  try {
    for (unsigned t = 1; t < std::min<std::size_t>(threads, items); ++t) pool.emplace_back(run);
  } catch (...) {
    failed.store(true);
    for (auto& thread : pool) thread.join();
    throw;
  }
  run();
  for (auto& thread : pool) thread.join();
  if (error) std::rethrow_exception(error);
}

inline void put_le(std::vector<char>& out, std::uint64_t value, unsigned bytes) {
  for (unsigned b = 0; b < bytes; ++b) out.push_back(static_cast<char>(value >> (8 * b)));
}

inline std::uint64_t get_le(const unsigned char* data, unsigned bytes) {
  std::uint64_t value = 0;
  for (unsigned b = 0; b < bytes; ++b) value |= static_cast<std::uint64_t>(data[b]) << (8 * b);
  return value;
}

struct LayerFile {
  std::string name;  // relative to the store directory; empty = not stored
  std::uint64_t bytes = 0, checksum = 0, nodes = 0;
  bool present() const { return !name.empty(); }
};

class LayerStore {
 public:
  enum Kind : unsigned { kForward = 0, kResult = 1 };
  static constexpr const char* kManifest = "manifest.twxman";

  LayerStore(std::string dir, int width, int height, unsigned length)
      : dir_(std::move(dir)), width_(width), height_(height), length_(length) {
    std::filesystem::create_directories(dir_);
  }

  static bool is_manifest(const std::string& path) {
    std::ifstream in(path);
    std::string first;
    return std::getline(in, first) && first == "TWXMAN01";
  }

  const std::string& dir() const { return dir_; }
  std::string manifest_path() const { return dir_ + "/" + kManifest; }
  unsigned phase = 0, cursor = 0, layer_count = 0;
  std::array<std::uint64_t, 5> root_key{};
  std::vector<LayerFile> forward, result;

  LayerFile& record(unsigned kind, std::size_t ply) {
    auto& files = kind == kForward ? forward : result;
    if (files.size() <= ply) files.resize(ply + 1);
    return files[ply];
  }
  bool has(unsigned kind, std::size_t ply) const {
    const auto& files = kind == kForward ? forward : result;
    return ply < files.size() && files[ply].present();
  }

  void save_manifest() const {
    const std::string path = manifest_path(), temporary = path + ".partial";
    {
      std::ofstream out(temporary, std::ios::trunc);
      out << "TWXMAN01\nwidth=" << width_ << "\nheight=" << height_ << "\norder=1\nlength=" << length_
          << "\nphase=" << phase << "\ncursor=" << cursor << "\nlayers=" << layer_count << "\nroot=";
      for (std::size_t i = 0; i < root_key.size(); ++i) out << (i ? "," : "") << root_key[i];
      out << '\n';
      for (unsigned kind : {kForward, kResult}) {
        const auto& files = kind == kForward ? forward : result;
        for (std::size_t ply = 0; ply < files.size(); ++ply) {
          if (!files[ply].present()) continue;
          out << (kind == kForward ? "forward " : "result ") << ply << ' ' << files[ply].name << ' '
              << files[ply].bytes << ' ' << files[ply].checksum << ' ' << files[ply].nodes << '\n';
        }
      }
      out << "end\n";
      out.flush();
      if (!out) throw std::runtime_error("cannot write layer manifest: " + temporary);
    }
    if (std::rename(temporary.c_str(), path.c_str()) != 0) {
      throw std::runtime_error("cannot replace layer manifest: " + path);
    }
  }

  // Parse a manifest; the store directory is the manifest's directory.
  static std::unique_ptr<LayerStore> open(const std::string& path) {
    std::ifstream in(path);
    std::string line;
    if (!std::getline(in, line) || line != "TWXMAN01") throw std::runtime_error("not a layer manifest: " + path);
    std::map<std::string, std::string> fields;
    std::vector<std::string> entries;
    bool ended = false;
    while (std::getline(in, line)) {
      if (line == "end") { ended = true; break; }
      const auto eq = line.find('=');
      if (eq != std::string::npos && line.compare(0, 8, "forward ") != 0 && line.compare(0, 7, "result ") != 0) {
        fields[line.substr(0, eq)] = line.substr(eq + 1);
      } else {
        entries.push_back(line);
      }
    }
    if (!ended) throw std::runtime_error("truncated layer manifest: " + path);
    auto number = [&](const char* key) -> std::uint64_t {
      const auto it = fields.find(key);
      if (it == fields.end()) throw std::runtime_error(std::string("layer manifest lacks ") + key);
      return std::stoull(it->second);
    };
    if (number("order") != 1) throw std::runtime_error("unknown DFA variable order in manifest");
    const std::string dir = std::filesystem::path(path).parent_path().string();
    auto store = std::make_unique<LayerStore>(dir.empty() ? "." : dir, static_cast<int>(number("width")),
                                              static_cast<int>(number("height")),
                                              static_cast<unsigned>(number("length")));
    store->phase = static_cast<unsigned>(number("phase"));
    store->cursor = static_cast<unsigned>(number("cursor"));
    store->layer_count = static_cast<unsigned>(number("layers"));
    std::istringstream root(fields.at("root"));
    for (auto& word : store->root_key) {
      std::string item;
      if (!std::getline(root, item, ',')) throw std::runtime_error("bad manifest root key");
      word = std::stoull(item);
    }
    for (const auto& entry : entries) {
      std::istringstream fields_in(entry);
      std::string kind;
      std::size_t ply = 0;
      LayerFile file;
      if (!(fields_in >> kind >> ply >> file.name >> file.bytes >> file.checksum >> file.nodes) ||
          (kind != "forward" && kind != "result")) {
        throw std::runtime_error("bad layer manifest entry: " + entry);
      }
      store->record(kind == "forward" ? kForward : kResult, ply) = file;
    }
    return store;
  }

  int width() const { return width_; }
  int height() const { return height_; }

  // Write the sub-DAG reachable from roots with compact local IDs.  Old
  // manager IDs are visited in increasing order, which is topological.
  // Format version 2 (version 1 files remain readable): nodes grouped by
  // level, deepest level first, and within a level by increasing local ID.
  // Local IDs are ranks of manager IDs plus 2, so every child precedes its
  // parent.  A record is a LEB128 local-ID delta (0 for a chunk's first
  // record), the arc mask, and the children's local IDs.  A directory after
  // the records lists every level's chunks (offset, first local ID, records)
  // so that chunks are encoded and interned in parallel; the file ends with
  // the directory offset and the checksum of everything before it.
  static constexpr std::uint64_t kChunkRecords = 1 << 16;
  void set_threads(unsigned threads) { threads_ = std::max(1U, threads); }

  void write(const twixt_dfa::Manager& dfa, unsigned kind, std::size_t ply, const std::vector<DfaId>& roots) {
    const auto& nodes = dfa.nodes();
    const DfaId base = dfa.base();
    const unsigned length = dfa.length();
    // Mark level by level; by_level[L] collects the level-L nodes.
    twixt_dfa::IdBitmap bits(base, dfa.size() - base);
    std::vector<std::vector<DfaId>> by_level(length);
    for (DfaId r : roots) {
      if (r >= 2 && bits.set(r)) by_level.at(nodes.level(r)).push_back(r);
    }
    for (unsigned level = 0; level + 1 < length; ++level) {
      const std::vector<DfaId>& here = by_level[level];
      const std::size_t chunks = (here.size() + kChunkRecords - 1) / kChunkRecords;
      std::vector<std::vector<DfaId>> found(chunks);
      parallel_items(threads_, chunks, [&](std::size_t c) {
        const std::size_t end = std::min<std::size_t>(here.size(), (c + 1) * kChunkRecords);
        for (std::size_t i = c * kChunkRecords; i < end; ++i) {
          for (DfaId child : nodes[here[i]].next) {
            if (child >= 2 && bits.set_atomic(child)) found[c].push_back(child);
          }
        }
      });
      std::vector<DfaId>& below = by_level[level + 1];
      for (auto& f : found) {
        below.insert(below.end(), f.begin(), f.end());
        std::vector<DfaId>().swap(f);
      }
    }
    parallel_items(threads_, length, [&](std::size_t level) { std::sort(by_level[level].begin(), by_level[level].end()); });
    const twixt_dfa::IdRank rank(bits);
    const std::uint64_t count = rank.count();
    auto local = [&](DfaId id) -> std::uint64_t { return id < 2 ? id : 2 + rank.rank(id); };
    // TWIXT_LAYER_ID_BYTES=5 forces wide local IDs (tests on small boards).
    static const bool wide = [] { const char* v = std::getenv("TWIXT_LAYER_ID_BYTES"); return v && std::string(v) == "5"; }();
    const unsigned id_bytes = count + 2 <= 0xffffffffULL && !wide ? 4 : 5;
    const std::string name = std::string(kind == kForward ? "forward-" : "result-") + std::to_string(ply) + ".twxlay";
    const std::string path = dir_ + "/" + name, temporary = path + ".partial";
    {
      BufferedOut out(temporary);
      out.put_raw("TWXLAY01", 8);
      for (std::uint64_t v : {2ULL, static_cast<unsigned long long>(width_), static_cast<unsigned long long>(height_),
                              1ULL, static_cast<unsigned long long>(length_), static_cast<unsigned long long>(ply),
                              static_cast<unsigned long long>(kind), static_cast<unsigned long long>(roots.size()),
                              static_cast<unsigned long long>(id_bytes)}) {
        out.put(v, 4);
      }
      out.put(count, 8);
      for (DfaId r : roots) out.put(local(r), 8);
      struct Chunk { std::uint64_t offset, first, records; };
      struct Block { unsigned level; std::uint64_t count; std::vector<Chunk> chunks; };
      std::vector<Block> blocks;
      std::uint64_t written = 0;
      for (unsigned level = length; level-- > 0;) {
        const std::vector<DfaId>& list = by_level[level];
        if (list.empty()) continue;
        Block block{level, list.size(), {}};
        const std::size_t chunks = (list.size() + kChunkRecords - 1) / kChunkRecords;
        const std::size_t round = 2 * std::size_t{threads_};
        for (std::size_t first = 0; first < chunks; first += round) {
          const std::size_t n = std::min(round, chunks - first);
          std::vector<std::vector<char>> encoded(n);
          parallel_items(threads_, n, [&](std::size_t k) {
            const std::size_t begin = (first + k) * kChunkRecords;
            const std::size_t end = std::min<std::size_t>(list.size(), begin + kChunkRecords);
            std::vector<char>& buffer = encoded[k];
            buffer.reserve((end - begin) * (2 + 3 * id_bytes));
            std::uint64_t previous = local(list[begin]);
            for (std::size_t i = begin; i < end; ++i) {
              const std::uint64_t self = local(list[i]);
              for (std::uint64_t delta = self - previous;; delta >>= 7) {
                buffer.push_back(static_cast<char>((delta & 0x7f) | (delta >= 0x80 ? 0x80 : 0)));
                if (delta < 0x80) break;
              }
              previous = self;
              const twixt_dfa::Node node = nodes[list[i]];
              unsigned mask = 0;
              for (unsigned c = 0; c < 3; ++c) if (node.next[c] != 0) mask |= 1U << c;
              buffer.push_back(static_cast<char>(mask));
              for (DfaId child : node.next) if (child != 0) put_le(buffer, local(child), id_bytes);
            }
          });
          for (std::size_t k = 0; k < n; ++k) {
            const std::size_t begin = (first + k) * kChunkRecords;
            const std::size_t end = std::min<std::size_t>(list.size(), begin + kChunkRecords);
            block.chunks.push_back(Chunk{out.position(), local(list[begin]), end - begin});
            out.put_block(encoded[k].data(), encoded[k].size());
            written += end - begin;
          }
        }
        blocks.push_back(std::move(block));
      }
      if (written != count) throw std::logic_error("layer writer lost nodes");
      const std::uint64_t directory = out.position();
      out.put(blocks.size(), 4);
      for (const Block& block : blocks) {
        out.put(block.level, 4);
        out.put(block.count, 8);
        out.put(block.chunks.size(), 8);
        for (const Chunk& chunk : block.chunks) {
          out.put(chunk.offset, 8);
          out.put(chunk.first, 8);
          out.put(chunk.records, 8);
        }
      }
      out.put(directory, 8);
      LayerFile& file = record(kind, ply);
      file.checksum = out.finish();
      file.bytes = out.bytes();
      file.nodes = count;
    }
    if (std::rename(temporary.c_str(), path.c_str()) != 0) throw std::runtime_error("cannot finish layer file: " + path);
    record(kind, ply).name = name;
  }

  // Intern a stored layer into dfa (sharing nodes already present there) and
  // return its roots as manager IDs.
  std::vector<DfaId> read(twixt_dfa::Manager& dfa, unsigned kind, std::size_t ply) const {
    if (!has(kind, ply)) throw std::runtime_error("layer " + std::to_string(ply) + " is not stored");
    const LayerFile& file = (kind == kForward ? forward : result)[ply];
    const std::string path = dir_ + "/" + file.name;
    if (std::filesystem::file_size(path) != file.bytes) throw std::runtime_error("layer file size mismatch: " + path);
    if (file_version(path) == 2) return read_v2(dfa, kind, ply, file, path);
    BufferedIn in(path);
    char magic[8];
    for (char& c : magic) c = static_cast<char>(in.get(1));
    if (std::string(magic, 8) != "TWXLAY01" || in.get(4) != 1) throw std::runtime_error("not a TWXLAY01 file: " + path);
    const std::uint64_t width = in.get(4), height = in.get(4), order = in.get(4), length = in.get(4);
    const std::uint64_t file_ply = in.get(4), file_kind = in.get(4), root_count = in.get(4), id_bytes = in.get(4);
    const std::uint64_t count = in.get(8);
    if (width != static_cast<std::uint64_t>(width_) || height != static_cast<std::uint64_t>(height_) || order != 1 ||
        length != dfa.length() || file_ply != ply || file_kind != kind || (id_bytes != 4 && id_bytes != 5) ||
        count != file.nodes) {
      throw std::runtime_error("layer file does not match its manifest entry: " + path);
    }
    std::vector<std::uint64_t> local_roots(root_count);
    for (auto& r : local_roots) r = in.get(8);
    std::vector<DfaId> mapped(count + 2);
    mapped[1] = 1;
    for (std::uint64_t i = 0; i < count; ++i) {
      const unsigned mask = static_cast<unsigned>(in.get(1));
      if (mask == 0 || mask > 7) throw std::runtime_error("invalid arc mask in " + path);
      twixt_dfa::Children children{};
      unsigned level = 0;
      for (unsigned c = 0; c < 3; ++c) {
        if ((mask & (1U << c)) == 0) continue;
        const std::uint64_t child = in.get(id_bytes);
        if (child == 0 || child >= i + 2) throw std::runtime_error("invalid topological order in " + path);
        children[c] = mapped[child];
        const unsigned child_level = child == 1 ? dfa.length() : dfa.node(children[c]).level;
        if (child_level == 0) throw std::runtime_error("invalid transition level in " + path);
        level = child_level - 1;
      }
      mapped[i + 2] = dfa.intern(level, children);
    }
    in.finish();
    std::vector<DfaId> roots;
    for (std::uint64_t r : local_roots) {
      if (r >= count + 2) throw std::runtime_error("invalid layer root in " + path);
      roots.push_back(mapped[r]);
    }
    return roots;
  }

  static unsigned file_version(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    char head[12];
    if (!in.read(head, 12) || std::string(head, 8) != "TWXLAY01") throw std::runtime_error("not a TWXLAY01 file: " + path);
    return static_cast<unsigned>(get_le(reinterpret_cast<const unsigned char*>(head + 8), 4));
  }

  std::vector<DfaId> read_v2(twixt_dfa::Manager& dfa, unsigned kind, std::size_t ply, const LayerFile& file,
                             const std::string& path) const {
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) throw std::runtime_error("cannot open layer file: " + path);
    struct stat info {};
    if (::fstat(fd, &info) != 0) {
      ::close(fd);
      throw std::runtime_error("cannot stat layer file: " + path);
    }
    const std::size_t size = static_cast<std::size_t>(info.st_size);
    void* map = size >= 16 ? ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0) : MAP_FAILED;
    ::close(fd);
    if (map == MAP_FAILED) throw std::runtime_error("cannot map layer file: " + path);
    struct Unmap {
      void* p;
      std::size_t n;
      ~Unmap() { ::munmap(p, n); }
    } unmap{map, size};
    const unsigned char* data = static_cast<const unsigned char*>(map);
    ::madvise(map, size, MADV_SEQUENTIAL);
    {
      Checksum sum;
      sum.update(reinterpret_cast<const char*>(data), size - 8);
      if (get_le(data + size - 8, 8) != sum.value()) throw std::runtime_error("layer file checksum mismatch: " + path);
    }
    auto fail = [&](const char* what) { throw std::runtime_error(std::string(what) + " in " + path); };
    std::size_t at = 8;
    const std::size_t content = size - 16;  // before the directory offset and checksum
    auto get = [&](unsigned bytes) {
      if (at + bytes > content) fail("truncated layer header");
      const std::uint64_t value = get_le(data + at, bytes);
      at += bytes;
      return value;
    };
    if (get(4) != 2) fail("unexpected layer version");
    const std::uint64_t width = get(4), height = get(4), order = get(4), length = get(4);
    const std::uint64_t file_ply = get(4), file_kind = get(4), root_count = get(4), id_bytes = get(4);
    const std::uint64_t count = get(8);
    if (width != static_cast<std::uint64_t>(width_) || height != static_cast<std::uint64_t>(height_) || order != 1 ||
        length != dfa.length() || file_ply != ply || file_kind != kind || (id_bytes != 4 && id_bytes != 5) ||
        count != file.nodes) {
      throw std::runtime_error("layer file does not match its manifest entry: " + path);
    }
    std::vector<std::uint64_t> local_roots(root_count);
    for (auto& r : local_roots) r = get(8);
    const std::size_t records_begin = at;
    const std::uint64_t directory = get_le(data + size - 16, 8);
    if (directory < records_begin || directory > content) fail("invalid layer directory offset");
    struct Chunk { std::uint64_t offset, first, records, end = 0; };
    struct Block { unsigned level; std::uint64_t count; std::vector<Chunk> chunks; };
    std::vector<Block> blocks;
    at = directory;
    const std::uint64_t block_count = get(4);
    std::uint64_t total = 0;
    for (std::uint64_t b = 0; b < block_count; ++b) {
      Block block;
      block.level = static_cast<unsigned>(get(4));
      block.count = get(8);
      const std::uint64_t chunks = get(8);
      if (block.level >= length || (!blocks.empty() && block.level >= blocks.back().level)) fail("invalid layer level order");
      if (chunks > (content - at) / 24) fail("truncated layer directory");
      std::uint64_t records = 0;
      for (std::uint64_t c = 0; c < chunks; ++c) {
        const std::uint64_t offset = get(8), first = get(8), n = get(8);
        if (n == 0 || n > kChunkRecords) fail("invalid layer chunk");
        block.chunks.push_back(Chunk{offset, first, n});
        records += n;
      }
      if (records != block.count || block.count == 0) fail("layer block size mismatch");
      total += block.count;
      blocks.push_back(std::move(block));
    }
    if (at != content) fail("layer directory has trailing data");
    if (total != count) fail("layer node count mismatch");
    // Chunks tile the record area in file order.
    std::vector<Chunk*> tiles;
    for (Block& block : blocks) for (Chunk& chunk : block.chunks) tiles.push_back(&chunk);
    if ((tiles.empty() ? directory : tiles.front()->offset) != records_begin) fail("layer records do not follow the header");
    for (std::size_t i = 0; i < tiles.size(); ++i) {
      tiles[i]->end = i + 1 < tiles.size() ? tiles[i + 1]->offset : directory;
      if (tiles[i]->end <= tiles[i]->offset) fail("invalid layer chunk offsets");
    }
    ::madvise(map, size, MADV_WILLNEED);
    std::vector<DfaId> mapped(count + 2, 0);
    mapped[1] = 1;
    for (const Block& block : blocks) {
      std::vector<std::uint64_t> start(block.chunks.size() + 1, 0);
      for (std::size_t c = 0; c < block.chunks.size(); ++c) start[c + 1] = start[c] + block.chunks[c].records;
      std::vector<DfaId> results(block.count);
      std::vector<std::uint64_t> locals(block.count);
      // Children were published by earlier blocks; mapped is read-only here.
      auto decode = [&](std::size_t c) {
        const Chunk& chunk = block.chunks[c];
        std::size_t p = chunk.offset;
        std::uint64_t self = chunk.first;
        for (std::uint64_t r = 0; r < chunk.records; ++r) {
          std::uint64_t delta = 0;
          for (unsigned shift = 0;; shift += 7) {
            if (p >= chunk.end || shift > 63) fail("corrupt layer record");
            const unsigned char byte = data[p++];
            delta |= static_cast<std::uint64_t>(byte & 0x7f) << shift;
            if ((byte & 0x80) == 0) break;
          }
          if ((r == 0) != (delta == 0)) fail("invalid layer local ID order");
          self += delta;
          if (self < 2 || self >= count + 2) fail("invalid layer local ID");
          if (p >= chunk.end) fail("corrupt layer record");
          const unsigned mask = data[p++];
          if (mask == 0 || mask > 7) fail("invalid arc mask");
          twixt_dfa::Children children{};
          for (unsigned symbol = 0; symbol < 3; ++symbol) {
            if ((mask & (1U << symbol)) == 0) continue;
            if (p + id_bytes > chunk.end) fail("corrupt layer record");
            const std::uint64_t child = get_le(data + p, static_cast<unsigned>(id_bytes));
            p += id_bytes;
            if (child == 0 || child >= count + 2 || mapped[child] == 0) fail("layer child does not precede its parent");
            children[symbol] = mapped[child];
          }
          results[start[c] + r] = dfa.intern(block.level, children);
          locals[start[c] + r] = self;
        }
        if (p != chunk.end) fail("layer chunk size mismatch");
      };
      const std::size_t jobs = std::min<std::size_t>(block.chunks.size(), threads_);
      if (jobs <= 1) {
        for (std::size_t c = 0; c < block.chunks.size(); ++c) decode(c);
      } else {
        twixt_dfa::ProcessOptions options;
        options.workers = static_cast<unsigned>(jobs);
        options.threads = true;
        twixt_dfa::thread_jobs(dfa, options, jobs, 0,
            [&](std::size_t job) {
              for (std::size_t c = job; c < block.chunks.size(); c += jobs) decode(c);
              return std::vector<DfaId>{};
            },
            [](std::size_t, const std::vector<DfaId>&) {}, []() {});
      }
      for (std::uint64_t i = 0; i < block.count; ++i) {
        if (mapped[locals[i]] != 0) fail("duplicate layer local ID");
        mapped[locals[i]] = results[i];
      }
    }
    std::vector<DfaId> roots;
    for (std::uint64_t r : local_roots) {
      if (r >= count + 2 || (r >= 2 && mapped[r] == 0)) fail("invalid layer root");
      roots.push_back(mapped[r]);
    }
    return roots;
  }

 private:
  std::string dir_;
  int width_, height_;
  unsigned length_;
  unsigned threads_ = 1;
};

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
        dfa_(alphabet(variables_), options.dfa_max_nodes, [this]() { check_time(); }, options.dfa_id_base) {
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
  int cell_level(int cell) const { return cell_level_[cell]; }
  std::string won_stats() const {
    return "pairs " + std::to_string(last_won_memo_entries_) + ", frontier states " +
           std::to_string(last_won_frontiers_) + ", widest level " + std::to_string(last_won_widest_);
  }
  bool legal_move(int cell, Player player) const { return game_.legal_cell(cell, player); }
  const twixt_dfa::Manager& manager() const { return dfa_; }
  std::vector<SymbolicLayer>& layers() { return layers_; }
  const std::vector<SymbolicLayer>& layers() const { return layers_; }
  const State& root_state() const { return root_; }
  void set_checkpoint_hook(std::function<void(unsigned, unsigned)> hook) {
    checkpoint_hook_ = std::move(hook);
  }
  void set_layer_store(std::unique_ptr<LayerStore> store) {
    store_ = std::move(store);
    if (store_) store_->set_threads(level_threads());
  }
  LayerStore* layer_store() { return store_.get(); }
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

  // Return free allocator pages to the OS at quiescent points.  glibc keeps
  // memory freed by one thread in that thread's arena; other threads cannot
  // reuse it, so a serial peak (classification) or a collection otherwise
  // stays resident through the next parallel phase.
  void trim_heap(const char* where) {
    static const bool enabled = [] {
      const char* value = std::getenv("TWIXT_NO_TRIM");
      return value == nullptr || *value == '\0' || *value == '0';
    }();
    if (!enabled) return;
    const double started = Profile::get().seconds();
    const auto before = self_vsz_rss();
    ::malloc_trim(0);
    if (Profile::get().on()) {
      const auto after = self_vsz_rss();
      Profile::get().line("trim,where=%s,rss_before=%llu,rss_after=%llu,seconds=%.3f", where,
                          static_cast<unsigned long long>(before.second),
                          static_cast<unsigned long long>(after.second),
                          Profile::get().seconds() - started);
    }
  }

  void collect_roots(const std::vector<DfaId*>& extra, bool values_only = false) {
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
    dfa_.collect(roots, options_.workers);
    collected_size_ = dfa_.size();
    trim_heap("collect");
  }

  void collect(std::initializer_list<DfaId*> extra = {}, bool values_only = false) {
    collect_roots(std::vector<DfaId*>(extra), values_only);
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
    const bool profiling = Profile::get().on();
    std::uint64_t mark = profiling ? dfa_.created_here() : 0;
    double started = profiling ? Profile::get().seconds() : 0;
    auto step = [&](const char* kind, int k, DfaId result) {
      if (!profiling) return;
      const std::uint64_t now = dfa_.created_here();
      const double finished = Profile::get().seconds();
      Profile::get().line("step,ply=%d,job=%ld,move=%d,k=%d,kind=%s,created=%llu,root=%llu,new_reach=%llu,"
                          "filter_bytes=%zu,seconds=%.3f",
                          profile_ply, profile_job, move, k, kind,
                          static_cast<unsigned long long>(now - mark), static_cast<unsigned long long>(result),
                          static_cast<unsigned long long>(Profile::get().reach() ? new_reachable(result, wave_base_) : 0),
                          dfa_.filter_bytes_here(), finished - started);
      mark = now;
      started = finished;
    };
    DfaId current = dfa_.filter(source, allowed, cell_level_[move], 0, color(mover));
    step("filter", -1, current);
    const SymbolWord all = dfa_.unrestricted();
    for (int edge : game_.incident_edges(move)) {
      const SymbolWord condition = link_condition(move, edge, mover);
      current = dfa_.conditional_bit(current, condition, edge_level_[edge], false);
      step("cbit", edge_level_[edge], current);
    }
    const DfaId result = dfa_.filter(current, all, turn_level_, static_cast<unsigned>(mover),
                       static_cast<unsigned>(other(mover)));
    step("flip", turn_level_, result);
    return result;
  }

  // Exact single-pass image of one placement, equal to image() (and to a
  // preceding peg-cube filter) without interning any intermediate DFA.
  // Per incident edge, a 3-bit state records whether its link condition is
  // still possible before the link variable, or which outcome must still be
  // confirmed by crossing variables placed after it.  The neighbor cell always
  // precedes the link variable, and incident links never cross one another.
  enum : std::uint8_t { kPreOk = 0, kPreFalse = 1, kNeedZero = 2, kNeedOne = 3, kDone = 4 };
  // The move's effect on the variable order, shared by both fused images.
  struct ImagePlan {
    struct Role { std::uint8_t edge, kind; };  // kind: 0 neighbor, 1 crossing
    std::vector<std::vector<Role>> roles;      // per level
    std::vector<int> edge_at;                  // per level: incident link index or -1
    std::array<int, 8> edge_level{}, last_after{};
    unsigned color = 0, cell_level = 0, turn_level = 0;
    Player mover = Player::White;
    const SymbolWord* cube = nullptr;
  };
  ImagePlan image_plan(int move, Player mover, const SymbolWord* cube) const {
    ImagePlan plan;
    plan.color = color(mover);
    plan.mover = mover;
    plan.cube = cube;
    const auto& edges = game_.incident_edges(move);
    if (edges.size() > 8) throw std::logic_error("too many incident links for fused image");
    const unsigned levels = dfa_.length();
    plan.roles.resize(levels);
    plan.edge_at.assign(levels, -1);
    for (unsigned i = 0; i < edges.size(); ++i) {
      const int e = edges[i];
      plan.edge_level[i] = edge_level_[e];
      plan.edge_at[edge_level_[e]] = static_cast<int>(i);
      const Edge& endpoints = game_.edge(e);
      const int neighbor = endpoints.a == move ? endpoints.b : endpoints.a;
      if (cell_level_[neighbor] >= edge_level_[e]) throw std::logic_error("neighbor follows link variable");
      plan.roles[cell_level_[neighbor]].push_back({static_cast<std::uint8_t>(i), 0});
      plan.last_after[i] = -1;
      for (int x = 0; x < game_.edge_count(); ++x) {
        if (!game_.edges_cross(e, x)) continue;
        plan.roles[edge_level_[x]].push_back({static_cast<std::uint8_t>(i), 1});
        if (edge_level_[x] > edge_level_[e]) plan.last_after[i] = std::max(plan.last_after[i], edge_level_[x]);
      }
    }
    plan.cell_level = static_cast<unsigned>(cell_level_[move]);
    plan.turn_level = static_cast<unsigned>(turn_level_);
    return plan;
  }
  // Calls emit(symbol, child, state) for each output arc of `node` (at
  // `level`) in link state `state`: the image child is child's image in state.
  template <class Emit>
  void image_arcs(const ImagePlan& plan, unsigned level, const twixt_dfa::Node& node, std::uint32_t state,
                  Emit&& emit) const {
    auto get = [](std::uint32_t s, unsigned i) { return (s >> (3 * i)) & 7U; };
    auto set = [](std::uint32_t s, unsigned i, unsigned v) { return (s & ~(7U << (3 * i))) | (v << (3 * i)); };
    const auto& alphabet = dfa_.alphabet();
    const unsigned allowed = plan.cube ? (*plan.cube)[level] : (1U << alphabet[level]) - 1;
    if (level == plan.cell_level) {
      if ((allowed & 1U) != 0) emit(plan.color, node.next[0], state);
    } else if (level == plan.turn_level) {
      const unsigned before = static_cast<unsigned>(plan.mover);
      if ((allowed & (1U << before)) != 0) {
        emit(static_cast<unsigned>(other(plan.mover)), node.next[before], state);
      }
    } else if (plan.edge_at[level] >= 0) {
      const unsigned i = static_cast<unsigned>(plan.edge_at[level]);
      if ((allowed & 1U) != 0 && node.next[0] != 0) {
        const unsigned v = get(state, i);
        if (v == kPreFalse) {
          emit(0, node.next[0], set(state, i, kDone));
        } else if (v == kPreOk) {
          if (plan.last_after[i] < 0) {
            emit(1, node.next[0], set(state, i, kDone));
          } else {
            emit(1, node.next[0], set(state, i, kNeedZero));
            emit(0, node.next[0], set(state, i, kNeedOne));
          }
        } else {
          throw std::logic_error("fused image link state");
        }
      }
    } else {
      for (unsigned a = 0; a < alphabet[level]; ++a) {
        if (node.next[a] == 0 || (allowed & (1U << a)) == 0) continue;
        std::uint32_t next = state;
        bool reject = false;
        for (const ImagePlan::Role role : plan.roles[level]) {
          const unsigned i = role.edge;
          const unsigned v = get(next, i);
          if (role.kind == 0) {
            if (a != plan.color && v == kPreOk) next = set(next, i, kPreFalse);
          } else if (static_cast<int>(level) < plan.edge_level[i]) {
            if (a == 1 && v == kPreOk) next = set(next, i, kPreFalse);
          } else if (v == kNeedZero) {
            if (a == 1) { reject = true; break; }
            if (static_cast<int>(level) == plan.last_after[i]) next = set(next, i, kDone);
          } else if (v == kNeedOne) {
            if (a == 1) next = set(next, i, kDone);
            else if (static_cast<int>(level) == plan.last_after[i]) { reject = true; break; }
          }
        }
        if (!reject) emit(a, node.next[a], next);
      }
    }
  }

  DfaId fused_image(DfaId source, int move, Player mover, const SymbolWord* cube = nullptr) {
    if (!game_.legal_cell(move, mover) || source == 0) return 0;
    const ImagePlan plan = image_plan(move, mover, cube);
    twixt_dfa::FlatMap memo;
    std::function<DfaId(DfaId, std::uint32_t)> visit = [&](DfaId id, std::uint32_t state) -> DfaId {
      if (id < 2) return id;  // every obligation resolves at or before its last crossing
      const std::uint64_t key = (static_cast<std::uint64_t>(id) << 24) | state;
      if (DfaId known = 0; memo.find(key, known)) return known;
      const twixt_dfa::Node node = dfa_.node(id);
      twixt_dfa::Children out{};
      image_arcs(plan, node.level, node, state,
                 [&](unsigned symbol, DfaId child, std::uint32_t next) { out[symbol] = visit(child, next); });
      const DfaId result = dfa_.intern(node.level, out);
      memo.insert(key, result);
      return result;
    };
    const DfaId result = visit(source, 0);
    profile_fused_memo = memo.size();
    return result;
  }

  // fused_image() one level at a time inside one worker: a state is a
  // (node, link state) pair.  Memory follows the widest level instead of a
  // memo of every pair; the spill file holds the records.
  DfaId level_fused_image(DfaId source, int move, Player mover, const SymbolWord* cube = nullptr) {
    if (!game_.legal_cell(move, mover) || source == 0) return 0;
    if (source < 2) return source;
    const ImagePlan plan = image_plan(move, mover, cube);
    struct Pair { DfaId node; std::uint32_t state; };
    const unsigned first = dfa_.node(source).level;
    SpillFile spill(spill_path("image"));
    std::vector<SpillLevel> levels;
    std::vector<Pair> current{{source, 0}};
    std::uint64_t total = 0;
    std::vector<char> record;
    for (unsigned level = first; !current.empty(); ++level) {
      if (level >= dfa_.length()) throw std::logic_error("image pair beyond the last level");
      check_time();
      SpillLevel section;  // one shard: next_offset stays zero
      section.states = current.size();
      section.chunk = kLevelChunk;
      section.begin = spill.written();
      total += current.size();
      twixt_dfa::PairMap index;
      std::vector<Pair> next;
      for (std::size_t begin = 0; begin < current.size(); begin += kLevelChunk) {
        const std::size_t end = std::min<std::size_t>(current.size(), begin + kLevelChunk);
        record.clear();
        for (std::size_t i = begin; i < end; ++i) {
          const twixt_dfa::Node node = dfa_.node(current[i].node);
          unsigned mask = 0;
          std::array<std::uint64_t, 3> target{};
          image_arcs(plan, level, node, current[i].state, [&](unsigned symbol, DfaId child, std::uint32_t state) {
            if (child < 2) {
              if (child == 1) {
                mask |= 1U << symbol;
                target[symbol] = 1;
              }
              return;
            }
            const twixt_dfa::PairKey key{child, state};
            DfaId local = 0;
            if (!index.find(key, local)) {
              local = next.size();
              index.insert(key, local);
              next.push_back(Pair{child, state});
            }
            mask |= 9U << symbol;
            target[symbol] = local * kLevelShards;
          });
          put_record(record, mask, target);
        }
        section.runs.emplace_back(spill.append(record), record.size());
      }
      section.end = spill.written();
      levels.push_back(std::move(section));
      current.swap(next);
    }
    profile_fused_memo = total;
    return replay_spill(spill, levels, first, 1);
  }

  // Exact single-pass preimage, equal to preimage() of a cube-filtered
  // target.  Reading the PARENT word, each incident link bit of the child is
  // the link condition, which may depend on crossing variables placed after
  // the link.  Both child branches are then followed in parallel (a subset
  // construction over (target node, link state) pairs) until a crossing
  // variable or the last crossing decides which branch is genuine.  The two
  // branches are disjoint, so no pairwise union or difference is interned.
  // For preimage output symbol `a` at `level`, calls add(b, state') for each
  // input symbol b of a target node in link state `state` (the caller drops
  // arcs that are absent or outside `allowed`).
  template <class Add>
  void preimage_arcs(const ImagePlan& plan, unsigned level, unsigned a, std::uint32_t state,
                     const twixt_dfa::Node& node, unsigned allowed, Add&& add) const {
    auto get = [](std::uint32_t s, unsigned i) { return (s >> (3 * i)) & 7U; };
    auto set = [](std::uint32_t s, unsigned i, unsigned v) { return (s & ~(7U << (3 * i))) | (v << (3 * i)); };
    if (level == plan.cell_level) {
      if (a == 0) add(plan.color, state);
    } else if (level == plan.turn_level) {
      if (a == static_cast<unsigned>(plan.mover)) add(static_cast<unsigned>(other(plan.mover)), state);
    } else if (plan.edge_at[level] >= 0) {
      if (a != 0) return;
      const unsigned i = static_cast<unsigned>(plan.edge_at[level]);
      const unsigned v = get(state, i);
      if (v == kPreFalse) {
        add(0, set(state, i, kDone));
      } else if (v == kPreOk) {
        if (plan.last_after[i] < 0) {
          add(1, set(state, i, kDone));
        } else {
          add(1, set(state, i, kNeedZero));
          add(0, set(state, i, kNeedOne));
        }
      } else {
        throw std::logic_error("fused preimage link state");
      }
    } else {
      if (node.next[a] == 0 || (allowed & (1U << a)) == 0) return;
      std::uint32_t s2 = state;
      for (const ImagePlan::Role role : plan.roles[level]) {
        const unsigned i = role.edge;
        const unsigned v = get(s2, i);
        if (role.kind == 0) {
          if (a != plan.color && v == kPreOk) s2 = set(s2, i, kPreFalse);
        } else if (static_cast<int>(level) < plan.edge_level[i]) {
          if (a == 1 && v == kPreOk) s2 = set(s2, i, kPreFalse);
        } else if (v == kNeedZero) {
          if (a == 1) return;
          if (static_cast<int>(level) == plan.last_after[i]) s2 = set(s2, i, kDone);
        } else if (v == kNeedOne) {
          if (a == 1) s2 = set(s2, i, kDone);
          else if (static_cast<int>(level) == plan.last_after[i]) return;
        }
      }
      add(a, s2);
    }
  }
  // The cube restricts target symbols except at the moved cell.
  unsigned preimage_allowed(const ImagePlan& plan, unsigned level) const {
    const unsigned all = (1U << dfa_.alphabet()[level]) - 1;
    return !plan.cube || level == plan.cell_level ? all : (*plan.cube)[level];
  }
  static std::uint64_t pack_pair(DfaId id, std::uint32_t state) { return (static_cast<std::uint64_t>(id) << 24) | state; }

  DfaId fused_preimage(DfaId target, int move, Player mover, const SymbolWord* cube = nullptr) {
    if (!game_.legal_cell(move, mover) || target == 0) return 0;
    const ImagePlan plan = image_plan(move, mover, cube);
    const auto& alphabet = dfa_.alphabet();
    struct VectorHash {
      std::size_t operator()(const std::vector<std::uint64_t>& v) const noexcept {
        std::uint64_t h = v.size();
        for (std::uint64_t x : v) h = twixt_dfa::hash64(h ^ x);
        return h;
      }
    };
    twixt_dfa::FlatMap single;
    std::unordered_map<std::vector<std::uint64_t>, DfaId, VectorHash> multi;
    std::function<DfaId(std::vector<std::uint64_t>&)> visit = [&](std::vector<std::uint64_t>& set_) -> DfaId {
      std::sort(set_.begin(), set_.end());
      set_.erase(std::unique(set_.begin(), set_.end()), set_.end());
      if (set_.empty()) return 0;
      const DfaId first = static_cast<DfaId>(set_[0] >> 24);
      if (first == 1) return 1;  // terminal level; every obligation has resolved
      if (set_.size() == 1) {
        if (DfaId known = 0; single.find(set_[0], known)) return known;
      } else {
        const auto found = multi.find(set_);
        if (found != multi.end()) return found->second;
      }
      const unsigned level = dfa_.node(first).level;
      const unsigned allowed = preimage_allowed(plan, level);
      twixt_dfa::Children out{};
      std::vector<std::uint64_t> next;
      for (unsigned a = 0; a < alphabet[level]; ++a) {
        next.clear();
        for (const std::uint64_t packed : set_) {
          const twixt_dfa::Node node = dfa_.node(static_cast<DfaId>(packed >> 24));
          preimage_arcs(plan, level, a, static_cast<std::uint32_t>(packed & 0xffffffULL), node, allowed,
                        [&](unsigned b, std::uint32_t s2) {
                          if ((allowed & (1U << b)) != 0 && node.next[b] != 0) next.push_back(pack_pair(node.next[b], s2));
                        });
        }
        if (!next.empty()) {
          std::vector<std::uint64_t> child_set(next);
          out[a] = visit(child_set);
        }
      }
      const DfaId result = dfa_.intern(level, out);
      if (set_.size() == 1) single.insert(set_[0], result);
      else multi.emplace(set_, result);
      return result;
    };
    std::vector<std::uint64_t> start{pack_pair(target, 0)};
    const DfaId result = visit(start);
    profile_fused_memo = single.size() + multi.size();
    profile_fused_multi = multi.size();
    return result;
  }

  // Distinct tuples of 64-bit elements in an arena: tuple i is
  // ids[start[i] .. start[i + 1]); open addressing over tuple indices.
  struct TupleSet {
    static constexpr std::uint64_t kLocal = (std::uint64_t{1} << 40) - 1;
    std::vector<std::uint64_t> ids;
    std::vector<std::uint64_t> start{0};
    std::vector<std::uint64_t> slots;  // local + 1 below bit 40, hash tag above; 0 = empty
    std::uint64_t size() const { return start.size() - 1; }
    static std::uint64_t hash(const std::uint64_t* p, std::size_t n) {
      std::uint64_t h = twixt_dfa::hash64(n + 0x9e3779b97f4a7c15ULL);
      for (std::size_t i = 0; i < n; ++i) h = twixt_dfa::hash64(h ^ p[i]);
      return h;
    }
    std::uint64_t find_or_insert(const std::uint64_t* p, std::size_t n, std::uint64_t h) {
      if ((size() + 1) * 2 > slots.size()) {
        const std::size_t capacity = slots.empty() ? 1024 : slots.size() * 2;
        std::vector<std::uint64_t> grown(capacity, 0);
        for (std::uint64_t local = 0; local < size(); ++local) {
          const std::uint64_t g = hash(ids.data() + start[local], start[local + 1] - start[local]);
          std::size_t j = g & (capacity - 1);
          while (grown[j] != 0) j = (j + 1) & (capacity - 1);
          grown[j] = (local + 1) | (g & ~kLocal);
        }
        slots.swap(grown);
      }
      const std::size_t mask = slots.size() - 1;
      const std::uint64_t tag = h & ~kLocal;
      for (std::size_t j = h & mask;; j = (j + 1) & mask) {
        const std::uint64_t slot = slots[j];
        if (slot == 0) {
          const std::uint64_t local = size();
          if (local + 1 >= kLocal) throw twixt_dfa::Limit("too many tuples in one level table");
          ids.insert(ids.end(), p, p + n);
          start.push_back(ids.size());
          slots[j] = (local + 1) | tag;
          return local;
        }
        if ((slot & ~kLocal) != tag) continue;
        const std::uint64_t local = (slot & kLocal) - 1;
        const std::uint64_t b = start[local], e = start[local + 1];
        if (e - b == n && std::equal(p, p + n, ids.begin() + static_cast<std::ptrdiff_t>(b))) return local;
      }
    }
  };

  // fused_preimage() one level at a time inside one worker: a state is a
  // sorted set of packed (node, link state) pairs, as in the subset
  // construction.  Memory follows the widest level.
  DfaId level_fused_preimage(DfaId target, int move, Player mover, const SymbolWord* cube = nullptr) {
    if (!game_.legal_cell(move, mover) || target == 0) return 0;
    if (target == 1) return 1;
    const ImagePlan plan = image_plan(move, mover, cube);
    const auto& alphabet = dfa_.alphabet();
    const unsigned first = dfa_.node(target).level;
    SpillFile spill(spill_path("preimage"));
    std::vector<SpillLevel> levels;
    TupleSet current;
    current.ids = {pack_pair(target, 0)};
    current.start = {0, 1};
    std::uint64_t total = 0;
    std::vector<char> record;
    std::vector<std::uint64_t> kids;
    for (unsigned level = first; current.size() != 0; ++level) {
      if (level >= dfa_.length()) throw std::logic_error("preimage set beyond the last level");
      check_time();
      SpillLevel section;  // one shard: next_offset stays zero
      section.states = current.size();
      section.chunk = kLevelChunk;
      section.begin = spill.written();
      total += current.size();
      const unsigned allowed = preimage_allowed(plan, level);
      TupleSet next;
      for (std::uint64_t begin = 0; begin < current.size(); begin += kLevelChunk) {
        const std::uint64_t end = std::min<std::uint64_t>(current.size(), begin + kLevelChunk);
        record.clear();
        for (std::uint64_t i = begin; i < end; ++i) {
          unsigned mask = 0;
          std::array<std::uint64_t, 3> target_of{};
          for (unsigned a = 0; a < alphabet[level]; ++a) {
            kids.clear();
            for (std::uint64_t j = current.start[i]; j < current.start[i + 1]; ++j) {
              const std::uint64_t packed = current.ids[j];
              const twixt_dfa::Node node = dfa_.node(static_cast<DfaId>(packed >> 24));
              preimage_arcs(plan, level, a, static_cast<std::uint32_t>(packed & 0xffffffULL), node, allowed,
                            [&](unsigned b, std::uint32_t s2) {
                              if ((allowed & (1U << b)) != 0 && node.next[b] != 0) kids.push_back(pack_pair(node.next[b], s2));
                            });
            }
            if (kids.empty()) continue;
            std::sort(kids.begin(), kids.end());
            kids.erase(std::unique(kids.begin(), kids.end()), kids.end());
            mask |= 1U << a;
            if ((kids[0] >> 24) == 1) {  // the terminal level: every obligation has resolved
              target_of[a] = 1;
              continue;
            }
            mask |= 8U << a;
            target_of[a] = next.find_or_insert(kids.data(), kids.size(), TupleSet::hash(kids.data(), kids.size())) * kLevelShards;
          }
          put_record(record, mask, target_of);
        }
        section.runs.emplace_back(spill.append(record), record.size());
      }
      section.end = spill.written();
      levels.push_back(std::move(section));
      next.slots = std::vector<std::uint64_t>();
      current = std::move(next);
    }
    profile_fused_memo = total;
    profile_fused_multi = 0;
    return replay_spill(spill, levels, first, 1);
  }

  // fused_preimage(target, ...) intersected with `within`, in one
  // level-by-level pass: a state is a node of `within` followed by the sorted
  // subset of packed (node, link state) pairs.  The preimage is never built
  // outside `within` (where it is mostly unreachable positions), and no
  // intersection memo is needed.
  DfaId level_preimage_within(DfaId target, int move, Player mover, const SymbolWord* cube, DfaId within) {
    if (!game_.legal_cell(move, mover) || target == 0 || within == 0) return 0;
    if (target < 2 || within < 2 || dfa_.node(target).level != dfa_.node(within).level) {
      return dfa_.intersect(fused_preimage(target, move, mover, cube), within);
    }
    const ImagePlan plan = image_plan(move, mover, cube);
    const auto& alphabet = dfa_.alphabet();
    const unsigned first = dfa_.node(target).level;
    SpillFile spill(spill_path("within"));
    std::vector<SpillLevel> levels;
    TupleSet current;
    current.ids = {within, pack_pair(target, 0)};
    current.start = {0, 2};
    std::uint64_t total = 0;
    std::vector<char> record;
    std::vector<std::uint64_t> kids;
    for (unsigned level = first; current.size() != 0; ++level) {
      if (level >= dfa_.length()) throw std::logic_error("preimage state beyond the last level");
      check_time();
      SpillLevel section;  // one shard: next_offset stays zero
      section.states = current.size();
      section.chunk = kLevelChunk;
      section.begin = spill.written();
      total += current.size();
      const unsigned allowed = preimage_allowed(plan, level);
      TupleSet next;
      for (std::uint64_t begin = 0; begin < current.size(); begin += kLevelChunk) {
        const std::uint64_t end = std::min<std::uint64_t>(current.size(), begin + kLevelChunk);
        record.clear();
        for (std::uint64_t i = begin; i < end; ++i) {
          const std::uint64_t* tuple = current.ids.data() + current.start[i];
          const std::uint64_t n = current.start[i + 1] - current.start[i];
          const twixt_dfa::Node w = dfa_.node(tuple[0]);
          unsigned mask = 0;
          std::array<std::uint64_t, 3> target_of{};
          for (unsigned a = 0; a < alphabet[level]; ++a) {
            const DfaId w_child = w.next[a];
            if (w_child == 0) continue;
            kids.clear();
            kids.push_back(w_child);  // element 0: the node of `within`
            for (std::uint64_t j = 1; j < n; ++j) {
              const std::uint64_t packed = tuple[j];
              const twixt_dfa::Node node = dfa_.node(static_cast<DfaId>(packed >> 24));
              preimage_arcs(plan, level, a, static_cast<std::uint32_t>(packed & 0xffffffULL), node, allowed,
                            [&](unsigned b, std::uint32_t s2) {
                              if ((allowed & (1U << b)) != 0 && node.next[b] != 0) kids.push_back(pack_pair(node.next[b], s2));
                            });
            }
            if (kids.size() == 1) continue;  // empty preimage subset
            std::sort(kids.begin() + 1, kids.end());
            kids.erase(std::unique(kids.begin() + 1, kids.end()), kids.end());
            mask |= 1U << a;
            if ((kids[1] >> 24) == 1) {  // terminal level: every obligation has resolved
              if (w_child != 1) throw std::logic_error("preimage and restricting language lengths differ");
              target_of[a] = 1;
              continue;
            }
            if (w_child < 2) throw std::logic_error("preimage and restricting language lengths differ");
            mask |= 8U << a;
            target_of[a] = next.find_or_insert(kids.data(), kids.size(), TupleSet::hash(kids.data(), kids.size())) * kLevelShards;
          }
          put_record(record, mask, target_of);
        }
        section.runs.emplace_back(spill.append(record), record.size());
      }
      section.end = spill.written();
      levels.push_back(std::move(section));
      next.slots = std::vector<std::uint64_t>();
      current = std::move(next);
    }
    profile_fused_memo = total;
    profile_fused_multi = 0;
    return replay_spill(spill, levels, first, 1);
  }

  static bool use_fused_preimage() {
    static const bool fused = [] {
      const char* value = std::getenv("TWIXT_CLASSIC_PREIMAGE");
      return value == nullptr || *value == '\0' || *value == '0';
    }();
    return fused;
  }

  static bool use_fused_image() {
    static const bool fused = [] {
      const char* value = std::getenv("TWIXT_CLASSIC_IMAGE");
      return value == nullptr || *value == '\0' || *value == '0';
    }();
    return fused;
  }

  // Count nodes reachable from root whose IDs are at least base (diagnostics).
  // Children precede parents, so older nodes cannot lead back to newer ones.
  std::uint64_t new_reachable(DfaId root, std::size_t base) const {
    if (root < 2 || root < base) return 0;
    std::vector<std::uint8_t> seen(root - base + 1);
    std::vector<DfaId> stack{root};
    seen[root - base] = 1;
    std::uint64_t count = 0;
    while (!stack.empty()) {
      const DfaId id = stack.back();
      stack.pop_back();
      ++count;
      for (DfaId child : dfa_.nodes()[id].next) {
        if (child >= 2 && child >= base && !seen[child - base]) {
          seen[child - base] = 1;
          stack.push_back(child);
        }
      }
    }
    return count;
  }

  void census_line(const char* label, long ply, long wave) {
    if (!Profile::get().on()) return;
    const double started = Profile::get().seconds();
    const auto c = dfa_.census(Profile::get().scan());
    const auto [vsz, rss] = self_vsz_rss();
    std::size_t arena = 0, mmapped = 0, in_use = 0, free_bytes = 0;
    if (Profile::get().mallinfo()) {
      const struct mallinfo2 info = ::mallinfo2();
      arena = info.arena; mmapped = info.hblkhd; in_use = info.uordblks; free_bytes = info.fordblks;
    }
    Profile::get().line("census,label=%s,ply=%ld,wave=%ld,slots=%zu,unused=%zu,blocks=%zu,"
                        "unique_entries=%zu,unique_buckets=%zu,filter_bytes=%zu,filter_used=%zu,"
                        "apply_u=%zu,apply_u_buckets=%zu,apply_i=%zu,apply_i_buckets=%zu,"
                        "apply_d=%zu,apply_d_buckets=%zu,products=%zu,product_buckets=%zu,"
                        "created=%llu,discarded=%llu,tail=%llu,rss=%llu,vsz=%llu,"
                        "m_arena=%zu,m_mmap=%zu,m_inuse=%zu,m_free=%zu,unique_bytes=%zu,census_seconds=%.3f",
                        label, ply, wave, c.slots, c.unused_slots, c.blocks,
                        c.unique_entries, c.unique_buckets, c.filter_bytes, c.filter_used_bytes,
                        c.apply_entries[0], c.apply_buckets[0], c.apply_entries[1], c.apply_buckets[1],
                        c.apply_entries[2], c.apply_buckets[2], c.product_entries, c.product_buckets,
                        static_cast<unsigned long long>(c.created), static_cast<unsigned long long>(c.discarded),
                        static_cast<unsigned long long>(c.reserved_tail),
                        static_cast<unsigned long long>(rss), static_cast<unsigned long long>(vsz),
                        arena, mmapped, in_use, free_bytes, c.unique_bytes, Profile::get().seconds() - started);
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
    if (!use_compact_won()) return classic_won(source, player);
    // Level-by-level evaluation bounds memory by the widest level; the
    // recursive memo is faster for small layers.  TWIXT_WON=level|compact.
    static const std::string mode = [] { const char* v = std::getenv("TWIXT_WON"); return v ? std::string(v) : std::string(); }();
    if (mode == "level" || (mode != "compact" && layer_nodes(source) >= 4'000'000)) {
      return level_won(source, player);
    }
    return compact_won(source, player);
  }

  DfaId classic_won(DfaId source, Player player) {
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
    const DfaId result = visit(source, {});
    last_won_memo_entries_ = memo.size();
    last_won_memo_buckets_ = memo.bucket_count();
    return result;
  }

  static bool use_compact_won() {
    static const bool compact = [] {
      const char* value = std::getenv("TWIXT_CLASSIC_WON");
      return value == nullptr || *value == '\0' || *value == '0';
    }();
    return compact;
  }

  // Same recognizer as won(), but frontier label arrays are interned once
  // and the memo maps (node, frontier ID) to the result in a flat table.
  using Components = std::array<std::uint8_t, kMaxCells>;
  struct ComponentsHash {
    std::size_t operator()(const Components& c) const noexcept {
      std::uint64_t h = 0x243f6a8885a308d3ULL;
      for (unsigned i = 0; i < kMaxCells; i += 8) {
        std::uint64_t word = 0;
        for (unsigned j = 0; j < 8; ++j) word |= static_cast<std::uint64_t>(c[i + j]) << (8 * j);
        h = twixt_dfa::hash64(h ^ word);
      }
      return h;
    }
  };
  // Connection frontier states: interned arrays of component labels.
  class FrontierTable {
   public:
    std::uint32_t intern(const Components& c) {
      const auto found = ids_.emplace(c, static_cast<std::uint32_t>(states_.size()));
      if (found.second) states_.push_back(c);
      return found.first->second;
    }
    const Components& get(std::uint32_t id) const { return states_[id]; }
    std::size_t size() const { return states_.size(); }
   private:
    std::unordered_map<Components, std::uint32_t, ComponentsHash> ids_;
    std::vector<Components> states_;
  };

  enum class FrontierStep { Dead, Connected, Continue };
  // One transition of the connection recognizer behind won(): Connected
  // accepts every completion of the child, Dead rejects, and Continue gives
  // the child's canonical frontier in `next`.
  FrontierStep frontier_step(const Components& components, unsigned level, unsigned symbol,
                             Player player, Components& next) const {
    next = components;
    const DfaVariable variable = variables_[level];
    if (!variable.link && variable.index >= 0 && symbol == color(player)) {
      next[variable.index] = static_cast<std::uint8_t>(
          63 | (start_border(variable.index, player) ? 64 : 0) |
          (target_border(variable.index, player) ? 128 : 0));
    } else if (variable.link && symbol == 1) {
      const Edge& edge = game_.edge(variable.index);
      const unsigned a = next[edge.a], b = next[edge.b];
      if (a != 0 && b != 0) {
        const unsigned merged = (a & 63) | ((a | b) & 192);
        if ((merged & 192) == 192) return FrontierStep::Connected;
        for (auto& label : next) {
          if (label != 0 && ((label & 63) == (a & 63) || (label & 63) == (b & 63))) {
            label = static_cast<std::uint8_t>(merged);
          }
        }
      }
    }
    for (int cell : retire_[level]) next[cell] = 0;
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
    return has_start || future_start_[static_cast<int>(player)][level + 1] ? FrontierStep::Continue
                                                                           : FrontierStep::Dead;
  }

  // Same recognizer as won(), but frontier label arrays are interned once
  // and the memo maps (node, frontier ID) to the result in a flat table.
  DfaId compact_won(DfaId source, Player player) {
    FrontierTable frontiers;
    // Keyed by (node, frontier ID), both up to 40 bits, in packed 16-byte
    // slots: frontier states grow about tenfold per ply (1.02M at 6x6 ply 16).
    twixt_dfa::PairMap memo;
    std::uint64_t visits = 0;
    std::function<DfaId(DfaId, const Components&)> visit;
    visit = [&](DfaId id, const Components& components) -> DfaId {
      if (id < 2) return 0;
      if (((++visits) & 0xfffULL) == 0) check_time();
      const twixt_dfa::PairKey key{id, frontiers.intern(components)};
      if (DfaId known = 0; memo.find(key, known)) return known;
      const twixt_dfa::Node node = dfa_.node(id);
      twixt_dfa::Children children{};
      Components next;
      for (unsigned symbol = 0; symbol < dfa_.alphabet()[node.level]; ++symbol) {
        if (node.next[symbol] == 0) continue;
        switch (frontier_step(components, node.level, symbol, player, next)) {
          case FrontierStep::Connected: children[symbol] = node.next[symbol]; break;
          case FrontierStep::Continue: children[symbol] = visit(node.next[symbol], next); break;
          case FrontierStep::Dead: break;
        }
      }
      const DfaId result = dfa_.intern(node.level, children);
      memo.insert(key, result);
      return result;
    };
    const DfaId result = visit(source, {});
    last_won_memo_entries_ = memo.size();
    last_won_frontiers_ = frontiers.size();
    return result;
  }

  // ---- Level-by-level constructions ----------------------------------------
  // The connection classifier and the n-ary union build their result one DFA
  // level at a time.  Phase 1 runs top-down: the distinct states of a level
  // (deduplicated in lock shards chosen by the hash's top bits) are expanded
  // in parallel chunks, and each chunk appends one record per state to a spill
  // file.  A record is a mask byte (bits 0-2: arc present; bits 3-5: the arc's
  // 5-byte value names a next-level state rather than a node) followed by the
  // values.  Phase 2 replays the levels bottom-up and interns the results,
  // keeping only the level below.  Memory follows the widest level instead of
  // every state of every level, and one shared table per level replaces
  // per-slice memos that recomputed common suffixes.
  // Lock shards for next-level states, chosen by the hash's top bits.
  static constexpr unsigned kLevelShardBits = 10, kLevelShards = 1U << kLevelShardBits;
  static unsigned level_shard(std::uint64_t hash) { return static_cast<unsigned>(hash >> (64 - kLevelShardBits)); }
  static constexpr std::size_t kLevelChunk = 1 << 16;
  struct SpillLevel {
    std::uint64_t states = 0, chunk = 0, begin = 0, end = 0;     // spill bytes [begin, end)
    std::vector<std::pair<std::uint64_t, std::uint64_t>> runs;  // per chunk: offset, bytes
    std::array<std::uint64_t, kLevelShards> next_offset{};      // dense start of each next shard
  };
  class SpillFile {
   public:
    explicit SpillFile(std::string path) : path_(std::move(path)), out_(path_, std::ios::binary | std::ios::trunc) {
      if (!out_) throw std::runtime_error("cannot create spill file: " + path_);
    }
    ~SpillFile() {
      out_.close();
      std::remove(path_.c_str());
    }
    SpillFile(const SpillFile&) = delete;
    SpillFile& operator=(const SpillFile&) = delete;
    std::uint64_t append(const std::vector<char>& data) {
      std::lock_guard<std::mutex> guard(mutex_);
      const std::uint64_t at = written_;
      out_.write(data.data(), static_cast<std::streamsize>(data.size()));
      if (!out_) throw std::runtime_error("failed writing spill file: " + path_);
      written_ += data.size();
      return at;
    }
    std::uint64_t written() const { return written_; }
    void finish() {
      out_.close();
      if (!out_) throw std::runtime_error("failed closing spill file: " + path_);
    }
    const std::string& path() const { return path_; }
   private:
    std::string path_;
    std::ofstream out_;
    std::mutex mutex_;
    std::uint64_t written_ = 0;
  };
  static void put_record(std::vector<char>& record, unsigned mask, const std::array<std::uint64_t, 3>& value) {
    record.push_back(static_cast<char>(mask));
    for (unsigned symbol = 0; symbol < 3; ++symbol) {
      if ((mask & (1U << symbol)) == 0) continue;
      if (value[symbol] > twixt_dfa::kMaxId) throw twixt_dfa::Limit("level spill value exceeds 40 bits");
      for (int b = 0; b < 5; ++b) record.push_back(static_cast<char>(value[symbol] >> (8 * b)));
    }
  }
  // About eight work items per thread, so small levels still spread.
  static std::size_t level_chunk(std::size_t states, unsigned threads) {
    return std::clamp<std::size_t>(states / (8 * std::size_t{threads}) + 1, 64, kLevelChunk);
  }
  static void run_items(unsigned threads, std::size_t items, const std::function<void(std::size_t)>& body) {
    parallel_items(threads, items, body);
  }
  // Phase 2 of a level construction whose first level holds one state.
  DfaId replay_spill(SpillFile& spill, const std::vector<SpillLevel>& levels, unsigned first, unsigned threads) {
    spill.finish();
    std::ifstream in(spill.path(), std::ios::binary);
    if (!in) throw std::runtime_error("cannot reopen spill file: " + spill.path());
    std::vector<DfaId> below;
    for (std::size_t s = levels.size(); s-- > 0;) {
      const SpillLevel& section = levels[s];
      const unsigned level = first + static_cast<unsigned>(s);
      std::vector<char> buffer(section.end - section.begin);
      in.seekg(static_cast<std::streamoff>(section.begin));
      if (!in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()))) {
        throw std::runtime_error("truncated spill file: " + spill.path());
      }
      std::vector<DfaId> results(section.states);
      const bool last = s + 1 == levels.size();
      const std::size_t chunks = section.runs.size();
      auto evaluate = [&](std::size_t c) {
        std::size_t at = section.runs[c].first - section.begin;
        const std::size_t stop = at + section.runs[c].second;
        const std::size_t end = std::min<std::size_t>(results.size(), (c + 1) * section.chunk);
        std::size_t i = c * section.chunk;
        for (; at < stop && i < end; ++i) {
          const unsigned mask = static_cast<unsigned char>(buffer[at++]);
          twixt_dfa::Children children{};
          for (unsigned symbol = 0; symbol < 3; ++symbol) {
            if ((mask & (1U << symbol)) == 0) continue;
            std::uint64_t value = 0;
            for (int b = 0; b < 5; ++b) value |= static_cast<std::uint64_t>(static_cast<unsigned char>(buffer[at++])) << (8 * b);
            if ((mask & (8U << symbol)) == 0) {
              children[symbol] = value;
              continue;
            }
            if (last) throw std::logic_error("level spill references a missing level");
            const std::uint64_t dense = section.next_offset[value % kLevelShards] + value / kLevelShards;
            if (dense >= below.size()) throw std::logic_error("level spill references a missing state");
            children[symbol] = below[dense];
          }
          results[i] = dfa_.intern(level, children);
        }
        if (at != stop || i != end) throw std::logic_error("level spill chunk size mismatch");
      };
      const std::size_t jobs = std::min<std::size_t>(chunks, threads);
      if (jobs <= 1) {
        for (std::size_t c = 0; c < chunks; ++c) evaluate(c);
      } else {
        twixt_dfa::ProcessOptions popts;
        popts.workers = static_cast<unsigned>(jobs);
        popts.threads = true;
        twixt_dfa::thread_jobs(dfa_, popts, jobs, 0,
            [&](std::size_t job) {
              for (std::size_t c = job; c < chunks; c += jobs) evaluate(c);
              return std::vector<DfaId>{};
            },
            [](std::size_t, const std::vector<DfaId>&) {}, []() {});
      }
      below.swap(results);
    }
    if (below.size() != 1) throw std::logic_error("level spill has no single root");
    return below[0];
  }

  // The same connection recognizer as compact_won(), one level at a time;
  // a state is a (node, frontier) pair.
  DfaId level_won(DfaId source, Player player, unsigned threads = 0) {
    if (source < 2) return 0;
    if (threads == 0) threads = level_threads();
    constexpr unsigned kShards = kLevelShards, kFrontierShards = 64;
    struct Pair { DfaId node; std::uint64_t frontier; };
    // Frontier states.  An entry is written under its shard's lock before its
    // ID is published; lock-free readers only look up IDs from earlier levels,
    // which a thread join separates from the writes.
    struct FrontierShard {
      std::mutex mutex;
      std::unordered_map<Components, std::uint64_t, ComponentsHash> ids;
      std::vector<std::unique_ptr<Components[]>> blocks;
      std::array<Components*, 1 << 14> table{};
      std::uint64_t count = 0;
    };
    constexpr std::uint64_t kBlock = 1 << 12;
    std::vector<std::unique_ptr<FrontierShard>> frontier(kFrontierShards);
    for (auto& f : frontier) f = std::make_unique<FrontierShard>();
    auto intern_frontier = [&](const Components& c) -> std::uint64_t {
      const std::uint64_t h = ComponentsHash{}(c);
      const unsigned shard = static_cast<unsigned>(h >> 58);  // 64 frontier shards
      FrontierShard& f = *frontier[shard];
      std::lock_guard<std::mutex> guard(f.mutex);
      const auto found = f.ids.find(c);
      if (found != f.ids.end()) return found->second;
      const std::uint64_t local = f.count;
      if (local / kBlock >= f.table.size()) throw twixt_dfa::Limit("too many connection frontier states");
      if (local % kBlock == 0) {
        f.blocks.emplace_back(new Components[kBlock]);
        f.table[local / kBlock] = f.blocks.back().get();
      }
      f.table[local / kBlock][local % kBlock] = c;
      ++f.count;
      const std::uint64_t id = local * kFrontierShards + shard;
      f.ids.emplace(c, id);
      return id;
    };
    auto frontier_at = [&](std::uint64_t id) -> const Components& {
      const FrontierShard& f = *frontier[id % kFrontierShards];
      const std::uint64_t local = id / kFrontierShards;
      return f.table[local / kBlock][local % kBlock];
    };
    // Pairs of the next level, named local * kShards + shard while the level
    // runs; the dense order is shard-major.
    struct PairShard {
      std::mutex mutex;
      twixt_dfa::PairMap index;
      std::vector<Pair> pairs;
    };
    const unsigned first = dfa_.node(source).level;
    SpillFile spill(spill_path("won"));
    std::vector<SpillLevel> levels;
    std::uint64_t total_pairs = 0, widest = 0;
    std::vector<Pair> current{{source, intern_frontier(Components{})}};
    for (unsigned level = first; !current.empty(); ++level) {
      if (level >= dfa_.length()) throw std::logic_error("connection pair beyond the last level");
      check_time();
      SpillLevel section;
      section.states = current.size();
      section.begin = spill.written();
      total_pairs += current.size();
      widest = std::max<std::uint64_t>(widest, current.size());
      std::vector<std::unique_ptr<PairShard>> next(kShards);
      for (auto& n : next) n = std::make_unique<PairShard>();
      const std::size_t chunk = level_chunk(current.size(), threads);
      const std::size_t chunks = (current.size() + chunk - 1) / chunk;
      section.chunk = chunk;
      section.runs.resize(chunks);
      run_items(threads, chunks, [&](std::size_t c) {
        std::vector<char> record;
        record.reserve(chunk * 8);
        Components step;
        const std::size_t end = std::min<std::size_t>(current.size(), (c + 1) * chunk);
        for (std::size_t i = c * chunk; i < end; ++i) {
          const Pair pair = current[i];
          const twixt_dfa::Node node = dfa_.node(pair.node);
          const Components components = frontier_at(pair.frontier);
          unsigned mask = 0;
          std::array<std::uint64_t, 3> target{};
          for (unsigned symbol = 0; symbol < dfa_.alphabet()[level]; ++symbol) {
            const DfaId child = node.next[symbol];
            if (child == 0) continue;
            switch (frontier_step(components, level, symbol, player, step)) {
              case FrontierStep::Connected:
                mask |= 1U << symbol;
                target[symbol] = child;
                break;
              case FrontierStep::Continue: {
                if (child < 2) break;  // the game ended without this connection
                const twixt_dfa::PairKey key{child, intern_frontier(step)};
                const unsigned shard = level_shard(twixt_dfa::hash_key(key));
                PairShard& n = *next[shard];
                DfaId local = 0;
                {
                  std::lock_guard<std::mutex> guard(n.mutex);
                  if (!n.index.find(key, local)) {
                    local = n.pairs.size();
                    n.index.insert(key, local);
                    n.pairs.push_back(Pair{child, key.b});
                  }
                }
                mask |= 9U << symbol;
                target[symbol] = local * kShards + shard;
                break;
              }
              case FrontierStep::Dead: break;
            }
          }
          put_record(record, mask, target);
        }
        section.runs[c] = {spill.append(record), record.size()};
      });
      section.end = spill.written();
      std::vector<Pair>().swap(current);
      {
        std::uint64_t total_next = 0;
        for (unsigned shard = 0; shard < kShards; ++shard) {
          section.next_offset[shard] = total_next;
          total_next += next[shard]->pairs.size();
        }
        current.resize(total_next);
        run_items(threads, kShards, [&](std::size_t shard) {
          std::copy(next[shard]->pairs.begin(), next[shard]->pairs.end(), current.begin() + static_cast<std::ptrdiff_t>(section.next_offset[shard]));
          next[shard].reset();
        });
      }
      levels.push_back(std::move(section));
    }
    std::uint64_t frontier_states = 0;
    for (const auto& f : frontier) frontier_states += f->count;
    frontier.clear();
    const DfaId result = replay_spill(spill, levels, first, threads);
    last_won_memo_entries_ = total_pairs;
    last_won_frontiers_ = frontier_states;
    last_won_widest_ = widest;
    return result;
  }

  // n-ary union, one level at a time; a state is a sorted tuple of two or
  // more same-level nodes (one node or none resolves at once).
  DfaId level_union(std::vector<DfaId> roots, unsigned threads = 0) {
    if (threads == 0) threads = level_threads();
    roots.erase(std::remove(roots.begin(), roots.end(), 0), roots.end());
    std::sort(roots.begin(), roots.end());
    roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
    if (roots.empty()) return 0;
    if (roots.size() == 1) return roots[0];
    if (roots[0] < 2) return dfa_.unite_many(std::move(roots));
    const unsigned first = dfa_.node(roots[0]).level;
    for (DfaId root : roots) {
      if (dfa_.node(root).level != first) return dfa_.unite_many(std::move(roots));
    }
    ++level_union_runs_;
    constexpr unsigned kShards = kLevelShards;
    constexpr std::uint64_t kLocal = (std::uint64_t{1} << 40) - 1;
    struct Tuples {  // tuple i is ids[start[i] .. start[i + 1])
      std::vector<DfaId> ids;
      std::vector<std::uint64_t> start{0};
      std::uint64_t size() const { return start.size() - 1; }
    };
    struct TupleShard {
      std::mutex mutex;
      Tuples tuples;
      std::vector<std::uint64_t> slots;  // local + 1 below bit 40, hash tag above; 0 = empty
    };
    auto tuple_hash = [](const DfaId* ids, std::size_t n) {
      std::uint64_t h = twixt_dfa::hash64(n + 0x9e3779b97f4a7c15ULL);
      for (std::size_t i = 0; i < n; ++i) h = twixt_dfa::hash64(h ^ ids[i]);
      return h;
    };
    // Caller holds the shard's lock.
    auto find_or_insert = [&](TupleShard& s, const DfaId* ids, std::size_t n, std::uint64_t h) -> std::uint64_t {
      Tuples& t = s.tuples;
      if ((t.size() + 1) * 2 > s.slots.size()) {
        const std::size_t capacity = s.slots.empty() ? 1024 : s.slots.size() * 2;
        std::vector<std::uint64_t> slots(capacity, 0);
        for (std::uint64_t local = 0; local < t.size(); ++local) {
          const std::uint64_t g = tuple_hash(t.ids.data() + t.start[local], t.start[local + 1] - t.start[local]);
          std::size_t j = g & (capacity - 1);
          while (slots[j] != 0) j = (j + 1) & (capacity - 1);
          slots[j] = (local + 1) | (g & ~kLocal);
        }
        s.slots.swap(slots);
      }
      const std::size_t mask = s.slots.size() - 1;
      const std::uint64_t tag = h & ~kLocal;
      for (std::size_t j = h & mask;; j = (j + 1) & mask) {
        const std::uint64_t slot = s.slots[j];
        if (slot == 0) {
          const std::uint64_t local = t.size();
          if (local + 1 >= kLocal) throw twixt_dfa::Limit("too many union tuples in one shard");
          t.ids.insert(t.ids.end(), ids, ids + n);
          t.start.push_back(t.ids.size());
          s.slots[j] = (local + 1) | tag;
          return local;
        }
        if ((slot & ~kLocal) != tag) continue;
        const std::uint64_t local = (slot & kLocal) - 1;
        const std::uint64_t b = t.start[local], e = t.start[local + 1];
        if (e - b == n && std::equal(ids, ids + n, t.ids.begin() + static_cast<std::ptrdiff_t>(b))) return local;
      }
    };
    SpillFile spill(spill_path("union"));
    std::vector<SpillLevel> levels;
    std::uint64_t total = 0, widest = 0;
    double expand_seconds = 0, flatten_seconds = 0;
    Tuples current;
    current.ids = roots;
    current.start.push_back(roots.size());
    for (unsigned level = first; current.size() != 0; ++level) {
      if (level >= dfa_.length()) throw std::logic_error("union tuple beyond the last level");
      check_time();
      const auto expand_started = std::chrono::steady_clock::now();
      SpillLevel section;
      section.states = current.size();
      section.begin = spill.written();
      total += current.size();
      widest = std::max<std::uint64_t>(widest, current.size());
      std::vector<std::unique_ptr<TupleShard>> next(kShards);
      for (auto& n : next) n = std::make_unique<TupleShard>();
      const std::size_t chunk = level_chunk(current.size(), threads);
      const std::size_t chunks = (current.size() + chunk - 1) / chunk;
      section.chunk = chunk;
      section.runs.resize(chunks);
      const unsigned symbols = dfa_.alphabet()[level];
      run_items(threads, chunks, [&](std::size_t c) {
        std::vector<char> record;
        record.reserve(chunk * 12);
        std::array<std::vector<DfaId>, 3> kids;
        const std::size_t end = std::min<std::size_t>(current.size(), (c + 1) * chunk);
        for (std::size_t i = c * chunk; i < end; ++i) {
          for (auto& k : kids) k.clear();
          for (std::uint64_t j = current.start[i]; j < current.start[i + 1]; ++j) {
            const twixt_dfa::Node node = dfa_.node(current.ids[j]);
            for (unsigned symbol = 0; symbol < symbols; ++symbol) {
              if (node.next[symbol] != 0) kids[symbol].push_back(node.next[symbol]);
            }
          }
          unsigned mask = 0;
          std::array<std::uint64_t, 3> target{};
          for (unsigned symbol = 0; symbol < symbols; ++symbol) {
            auto& k = kids[symbol];
            if (k.empty()) continue;
            std::sort(k.begin(), k.end());
            k.erase(std::unique(k.begin(), k.end()), k.end());
            mask |= 1U << symbol;
            if (k.size() == 1) {
              target[symbol] = k[0];
              continue;
            }
            const std::uint64_t h = tuple_hash(k.data(), k.size());
            const unsigned shard = level_shard(h);
            TupleShard& n = *next[shard];
            std::uint64_t local = 0;
            {
              std::lock_guard<std::mutex> guard(n.mutex);
              local = find_or_insert(n, k.data(), k.size(), h);
            }
            mask |= 8U << symbol;
            target[symbol] = local * kShards + shard;
          }
          put_record(record, mask, target);
        }
        section.runs[c] = {spill.append(record), record.size()};
      });
      section.end = spill.written();
      const auto flatten_started = std::chrono::steady_clock::now();
      expand_seconds += std::chrono::duration<double>(flatten_started - expand_started).count();
      Tuples following;
      std::vector<std::uint64_t> id_offset(kShards + 1, 0);
      for (unsigned shard = 0; shard < kShards; ++shard) {
        section.next_offset[shard] = section.next_offset[shard == 0 ? 0 : shard - 1] +
                                     (shard == 0 ? 0 : next[shard - 1]->tuples.size());
        id_offset[shard + 1] = id_offset[shard] + next[shard]->tuples.ids.size();
      }
      const std::uint64_t count = section.next_offset[kShards - 1] + next[kShards - 1]->tuples.size();
      following.ids.resize(id_offset[kShards]);
      following.start.assign(count + 1, 0);
      run_items(threads, kShards, [&](std::size_t shard) {
        const Tuples& t = next[shard]->tuples;
        std::copy(t.ids.begin(), t.ids.end(), following.ids.begin() + static_cast<std::ptrdiff_t>(id_offset[shard]));
        const std::uint64_t first_tuple = section.next_offset[shard];
        for (std::uint64_t local = 0; local < t.size(); ++local) {
          following.start[first_tuple + local + 1] = id_offset[shard] + t.start[local + 1];
        }
        next[shard].reset();
      });
      flatten_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - flatten_started).count();
      levels.push_back(std::move(section));
      current = std::move(following);
    }
    const auto replay_started = std::chrono::steady_clock::now();
    const DfaId result = replay_spill(spill, levels, first, threads);
    last_union_tuples_ = total;
    last_union_widest_ = widest;
    last_union_phases_ = {expand_seconds, flatten_seconds,
                          std::chrono::duration<double>(std::chrono::steady_clock::now() - replay_started).count()};
    return result;
  }

  // Intersection or difference, one level at a time; a state is an operand
  // pair whose result is not yet decided by the terminal rules.
  enum class LevelOp { Intersect, Difference };
  DfaId level_binary(LevelOp op, DfaId a, DfaId b, unsigned threads = 0) {
    if (threads == 0) threads = level_threads();
    auto decided = [op](DfaId x, DfaId y, DfaId& out) {
      if (op == LevelOp::Intersect) {
        if (x == 0 || y == 0) return out = 0, true;
        if (x == y) return out = x, true;
      } else {
        if (x == 0 || x == y) return out = 0, true;
        if (y == 0) return out = x, true;
      }
      return false;
    };
    DfaId direct = 0;
    if (decided(a, b, direct)) return direct;
    if (a < 2 || b < 2 || dfa_.node(a).level != dfa_.node(b).level) {
      return op == LevelOp::Intersect ? dfa_.intersect(a, b) : dfa_.difference(a, b);
    }
    constexpr unsigned kShards = kLevelShards;
    struct Pair { DfaId a, b; };
    struct PairShard {
      std::mutex mutex;
      twixt_dfa::PairMap index;
      std::vector<Pair> pairs;
    };
    const unsigned first = dfa_.node(a).level;
    SpillFile spill(spill_path(op == LevelOp::Intersect ? "intersect" : "difference"));
    std::vector<SpillLevel> levels;
    std::vector<Pair> current{{a, b}};
    for (unsigned level = first; !current.empty(); ++level) {
      if (level >= dfa_.length()) throw std::logic_error("operand pair beyond the last level");
      check_time();
      SpillLevel section;
      section.states = current.size();
      section.begin = spill.written();
      std::vector<std::unique_ptr<PairShard>> next(kShards);
      for (auto& n : next) n = std::make_unique<PairShard>();
      const std::size_t chunk = level_chunk(current.size(), threads);
      const std::size_t chunks = (current.size() + chunk - 1) / chunk;
      section.chunk = chunk;
      section.runs.resize(chunks);
      const unsigned symbols = dfa_.alphabet()[level];
      run_items(threads, chunks, [&](std::size_t c) {
        std::vector<char> record;
        record.reserve(chunk * 12);
        const std::size_t end = std::min<std::size_t>(current.size(), (c + 1) * chunk);
        for (std::size_t i = c * chunk; i < end; ++i) {
          const twixt_dfa::Node x = dfa_.node(current[i].a), y = dfa_.node(current[i].b);
          unsigned mask = 0;
          std::array<std::uint64_t, 3> target{};
          for (unsigned symbol = 0; symbol < symbols; ++symbol) {
            DfaId out = 0;
            if (decided(x.next[symbol], y.next[symbol], out)) {
              if (out != 0) {
                mask |= 1U << symbol;
                target[symbol] = out;
              }
              continue;
            }
            const twixt_dfa::PairKey key{x.next[symbol], y.next[symbol]};
            const unsigned shard = level_shard(twixt_dfa::hash_key(key));
            PairShard& n = *next[shard];
            DfaId local = 0;
            {
              std::lock_guard<std::mutex> guard(n.mutex);
              if (!n.index.find(key, local)) {
                local = n.pairs.size();
                n.index.insert(key, local);
                n.pairs.push_back(Pair{key.a, key.b});
              }
            }
            mask |= 9U << symbol;
            target[symbol] = local * kShards + shard;
          }
          put_record(record, mask, target);
        }
        section.runs[c] = {spill.append(record), record.size()};
      });
      section.end = spill.written();
      std::vector<Pair>().swap(current);
      {
        std::uint64_t total_next = 0;
        for (unsigned shard = 0; shard < kShards; ++shard) {
          section.next_offset[shard] = total_next;
          total_next += next[shard]->pairs.size();
        }
        current.resize(total_next);
        run_items(threads, kShards, [&](std::size_t shard) {
          std::copy(next[shard]->pairs.begin(), next[shard]->pairs.end(), current.begin() + static_cast<std::ptrdiff_t>(section.next_offset[shard]));
          next[shard].reset();
        });
      }
      levels.push_back(std::move(section));
    }
    return replay_spill(spill, levels, first, threads);
  }

  // Threads for level-by-level operations and layer I/O (--level-threads,
  // default: the worker count).  Waves keep one task per worker.
  unsigned level_threads() const {
    return options_.level_threads ? options_.level_threads : std::max(1U, options_.workers);
  }

  // Whole-layer set operations: level-synchronous and parallel with workers
  // (TWIXT_LAYER_OPS=serial selects the memoized serial operations).
  bool level_layer_ops() const {
    static const bool serial = [] { const char* v = std::getenv("TWIXT_LAYER_OPS"); return v && std::string(v) == "serial"; }();
    return options_.workers > 1 && !options_.parallel_processes && !serial;
  }
  DfaId layer_union(DfaId a, DfaId b) { return level_layer_ops() ? level_union({a, b}) : dfa_.unite(a, b); }
  DfaId layer_difference(DfaId a, DfaId b) {
    return level_layer_ops() ? level_binary(LevelOp::Difference, a, b) : dfa_.difference(a, b);
  }

  std::string spill_path(const char* what) {
    const std::string dir = store_ ? store_->dir() : std::string(".");
    return dir + "/" + what + "-" + std::to_string(::getpid()) + "-" + std::to_string(++spill_counter_) + ".spill";
  }

  void add_layer(DfaId reachable) {
    const std::size_t ply = layers_.size();
    layers_.push_back(SymbolicLayer{});
    auto& layer = layers_.back();
    layer.reachable = reachable;
    const bool profiling = Profile::get().on();
    std::uint64_t mark = profiling ? dfa_.created_here() : 0;
    double started = profiling ? Profile::get().seconds() : 0;
    auto step = [&](const char* kind, DfaId result) {
      if (!profiling) return;
      const std::uint64_t now = dfa_.created_here();
      const auto [vsz, rss] = self_vsz_rss();
      const double finished = Profile::get().seconds();
      Profile::get().line("layer_step,ply=%zu,kind=%s,created=%llu,root=%llu,won_memo=%zu,won_buckets=%zu,"
                          "won_frontiers=%zu,rss=%llu,vsz=%llu,seconds=%.3f", ply, kind,
                          static_cast<unsigned long long>(now - mark), static_cast<unsigned long long>(result),
                          last_won_memo_entries_, last_won_memo_buckets_, last_won_frontiers_,
                          static_cast<unsigned long long>(rss), static_cast<unsigned long long>(vsz),
                          finished - started);
      mark = now;
      started = finished;
    };
    layer.terminal_loss = won(reachable, other(turn(ply)));
    step("won", layer.terminal_loss);
    SymbolWord no_moves = dfa_.unrestricted();
    for (int cell = 0; cell < game_.cell_count(); ++cell) {
      if (game_.legal_cell(cell, turn(ply))) no_moves[cell_level_[cell]] = 6;
    }
    const DfaId exhausted = dfa_.filter(reachable, no_moves);
    dfa_.release_worker_filter_buffers();  // coordinator scratch, sized by the root ID
    step("exhausted", exhausted);
    layer.terminal_draw = layer_difference(exhausted, layer.terminal_loss);
    step("terminal_draw", layer.terminal_draw);
    layer.active = layer_difference(reachable, layer_union(layer.terminal_loss, layer.terminal_draw));
    step("active", layer.active);
    trim_heap("add_layer");
    census_line("post_add_layer", static_cast<long>(ply), -1);
  }

  struct PlacementTask {
    int move = 0;
    SymbolWord cube;
  };

  bool use_workers(std::size_t ply) const {
    return options_.workers > 1 &&
           layer_nodes(layers_[ply].active) >= options_.parallel_min_nodes;
  }

  // Cubes are disjoint and cover the active language. Restrict only PEG
  // variables: translating a parent cube to child coordinates then changes
  // just the placed peg, even when the move creates several links.
  std::vector<PlacementTask> placement_tasks(std::size_t ply) {
    std::vector<int> moves;
    for (int cell = 0; cell < game_.cell_count(); ++cell) {
      if (game_.legal_cell(cell, turn(ply))) moves.push_back(cell);
    }
    const std::size_t wanted = (options_.workers + moves.size() - 1) / moves.size();
    std::vector<SymbolWord> cubes{dfa_.unrestricted()};
    for (unsigned level = 0; level < variables_.size() && cubes.size() < wanted; ++level) {
      if (variables_[level].link || variables_[level].index < 0) continue;
      std::vector<SymbolWord> split;
      for (const auto& cube : cubes) {
        for (unsigned symbol = 0; symbol < 3; ++symbol) {
          auto restricted = cube;
          restricted[level] = static_cast<std::uint8_t>(1U << symbol);
          if (dfa_.filter(layers_[ply].active, restricted) != 0) split.push_back(std::move(restricted));
        }
      }
      cubes = std::move(split);
    }
    std::vector<PlacementTask> tasks;
    for (const auto& cube : cubes) {
      for (int move : moves) {
        if ((cube[cell_level_[move]] & 1) != 0) tasks.push_back({move, cube});
      }
    }
    std::cout << "Parallel ply " << ply << ": " << options_.workers << " worker ceiling, "
              << tasks.size() << " tasks, " << cubes.size() << " disjoint peg shards\n";
    return tasks;
  }

  // `jobs`: the ply's task count; with --single-wave every task joins one
  // wave (one union and GC per ply) and the workers share them dynamically.
  twixt_dfa::ProcessOptions process_options(std::size_t jobs = 0) const {
    twixt_dfa::ProcessOptions options{options_.workers, options_.parallel_memory_gb * (1ULL << 30),
                                      options_.parallel_temp_directory, !options_.parallel_processes};
    if (options_.single_wave && !options_.parallel_processes) options.wave_jobs = jobs;
    return options;
  }

  static bool use_many_union() {
    static const bool many = [] {
      const char* value = std::getenv("TWIXT_CLASSIC_UNION");
      return value == nullptr || *value == '\0' || *value == '0';
    }();
    return many;
  }

  DfaId balanced_union(std::vector<DfaId> roots) {
    if (use_many_union()) return dfa_.unite_many(std::move(roots));
    return pairwise_union(std::move(roots));
  }

  DfaId pairwise_union(std::vector<DfaId> roots) {
    roots.erase(std::remove(roots.begin(), roots.end(), 0), roots.end());
    if (roots.empty()) return 0;
    while (roots.size() > 1) {
      std::vector<DfaId> next;
      next.reserve((roots.size() + 1) / 2);
      for (std::size_t i = 0; i < roots.size(); i += 2) {
        next.push_back(i + 1 == roots.size() ? roots[i] : dfa_.unite(roots[i], roots[i + 1]));
      }
      roots = std::move(next);
    }
    return roots[0];
  }

  // Union of a wave's results.  The level-synchronous union is the default;
  // TWIXT_UNION=slices selects the slice union below and TWIXT_UNION=check
  // computes both and requires the same root.
  DfaId parallel_union(std::vector<DfaId>& roots, const std::vector<DfaId*>& extra = {}) {
    (void)extra;  // no GC until the complete operation/wave has finished
    if (options_.parallel_processes) return balanced_union(roots);
    static const std::string mode = [] { const char* v = std::getenv("TWIXT_UNION"); return v ? std::string(v) : std::string(); }();
    if (mode == "slices") return slice_union(roots);
    const double started = Profile::get().on() ? Profile::get().seconds() : 0;
    last_union_tuples_ = last_union_widest_ = 0;
    const DfaId result = level_union(roots);
    if (Profile::get().on()) {
      Profile::get().line("level_union,operands=%zu,tuples=%llu,widest=%llu,expand=%.1f,flatten=%.1f,replay=%.1f,"
                          "seconds=%.3f", roots.size(), static_cast<unsigned long long>(last_union_tuples_),
                          static_cast<unsigned long long>(last_union_widest_), last_union_phases_[0],
                          last_union_phases_[1], last_union_phases_[2], Profile::get().seconds() - started);
    }
    if (mode == "check" && slice_union(roots) != result) throw std::logic_error("level union differs from slice union");
    return result;
  }

  // Expand a small prefix of the union automaton, solve independent suffix
  // languages in parallel, and stitch the prefix once. In particular the final
  // large union is parallel too; a binary reduction tree would serialize it.
  DfaId slice_union(std::vector<DfaId>& roots) {
    struct SignatureHash {
      std::size_t operator()(const std::vector<DfaId>& ids) const {
        std::uint64_t h = 0;
        for (DfaId id : ids) h = twixt_dfa::hash64(h ^ id);
        return h;
      }
    };
    struct Prefix {
      std::vector<DfaId> roots;
      std::array<std::size_t, 3> children{};
      unsigned level = 0;
      DfaId result = 0;
      bool expanded = false;
    };
    std::vector<Prefix> prefix;
    std::unordered_map<std::vector<DfaId>, std::size_t, SignatureHash> known;
    auto make = [&](std::vector<DfaId> ids, std::vector<std::size_t>& frontier) {
      ids.erase(std::remove(ids.begin(), ids.end(), 0), ids.end());
      std::sort(ids.begin(), ids.end());
      ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
      const auto found = known.find(ids);
      if (found != known.end()) return found->second;
      const std::size_t index = prefix.size();
      known.emplace(ids, index);
      prefix.push_back(Prefix{});
      prefix[index].roots = std::move(ids);
      if (prefix[index].roots.size() <= 1) {
        prefix[index].result = prefix[index].roots.empty() ? 0 : prefix[index].roots[0];
      } else {
        prefix[index].level = dfa_.node(prefix[index].roots[0]).level;
        frontier.push_back(index);
      }
      return index;
    };
    std::vector<std::size_t> frontier;
    make(roots, frontier);
    while (!frontier.empty() && frontier.size() < options_.workers) {
      std::vector<std::size_t> next;
      for (std::size_t index : frontier) {
        const unsigned level = prefix[index].level;
        prefix[index].expanded = true;
        for (unsigned c = 0; c < dfa_.alphabet()[level]; ++c) {
          std::vector<DfaId> children;
          for (DfaId root : prefix[index].roots) children.push_back(dfa_.node(root).next[c]);
          const std::size_t child = make(std::move(children), next);
          prefix[index].children[c] = child;
        }
      }
      frontier = std::move(next);
    }
    twixt_dfa::thread_jobs(dfa_, process_options(), frontier.size(), 1,
        [&](std::size_t job) {
          const double started = Profile::get().on() ? Profile::get().seconds() : 0;
          const DfaId result = balanced_union(prefix[frontier[job]].roots);
          if (Profile::get().on() && use_many_union()) {
            Profile::get().line("union_job,job=%zu,operands=%zu,pairs=%zu,trie=%zu,memo_bytes=%zu,seconds=%.3f",
                                job, prefix[frontier[job]].roots.size(), twixt_dfa::union_memo_pairs,
                                twixt_dfa::union_memo_trie, twixt_dfa::union_memo_bytes,
                                Profile::get().seconds() - started);
          }
          return std::vector<DfaId>{result};
        },
        [&](std::size_t job, const std::vector<DfaId>& result) { prefix[frontier[job]].result = result[0]; },
        []() {});
    for (std::size_t index = prefix.size(); index-- > 0;) {
      if (!prefix[index].expanded) continue;
      twixt_dfa::Children children{};
      for (unsigned c = 0; c < dfa_.alphabet()[prefix[index].level]; ++c) {
        children[c] = prefix[prefix[index].children[c]].result;
      }
      prefix[index].result = dfa_.intern(prefix[index].level, children);
    }
    return prefix[0].result;
  }

  void print_parallel_stats(const char* phase, std::size_t ply,
                             const twixt_dfa::ProcessStats& stats) const {
    std::cout << "Parallel " << phase << " ply " << ply << ": wall=" << stats.seconds
              << " s, import=" << stats.import_seconds << " s, merge/GC=" << stats.merge_seconds
              << " s, worker_slots=" << stats.peak_workers
              << ", peak_active_workers=" << stats.peak_active_workers
              << (options_.parallel_processes ? ", peak_PSS_GiB=" : ", peak_RSS_GiB=")
              << stats.peak_memory_bytes / static_cast<double>(1ULL << 30)
              << ", delta_MiB=" << stats.delta_bytes / static_cast<double>(1ULL << 20) << '\n';
  }

  DfaId parallel_forward(std::size_t ply) {
    const auto tasks = placement_tasks(ply);
    collect();
    wave_base_ = dfa_.size();
    // Level-by-level images for large layers (TWIXT_IMAGE=level|memo forces one).
    static const std::string image_mode = [] { const char* v = std::getenv("TWIXT_IMAGE"); return v ? std::string(v) : std::string(); }();
    level_image_ = image_mode == "level" || (image_mode != "memo" && dfa_.size() >= 50'000'000);
    long wave_index = 0;
    census_line("wave_start", static_cast<long>(ply), wave_index);
    DfaId next = 0;
    std::vector<DfaId> wave(tasks.size());
    double union_seconds = 0, gc_seconds = 0;
    std::size_t pool_before_gc = 0;
    const auto stats = twixt_dfa::parallel_jobs(dfa_, process_options(tasks.size()), tasks.size(), 1,
        [&](std::size_t job) {
          const auto& task = tasks[job];
          profile_ply = static_cast<int>(ply);
          profile_job = static_cast<long>(job);
          const bool profiling = Profile::get().on();
          const std::uint64_t created = profiling ? dfa_.created_here() : 0;
          const double started = profiling ? Profile::get().seconds() : 0;
          DfaId result = 0;
          if (use_fused_image()) {
            result = level_image_ ? level_fused_image(layers_[ply].active, task.move, turn(ply), &task.cube)
                                  : fused_image(layers_[ply].active, task.move, turn(ply), &task.cube);
            if (profiling) {
              Profile::get().line("step,ply=%zu,job=%zu,move=%d,k=-3,kind=fused,created=%llu,root=%llu,new_reach=0,"
                                  "filter_bytes=%zu,seconds=%.3f,memo=%zu", ply, job, task.move,
                                  static_cast<unsigned long long>(dfa_.created_here() - created), static_cast<unsigned long long>(result),
                                  dfa_.filter_bytes_here(), Profile::get().seconds() - started,
                                  profile_fused_memo);
            }
          } else {
          const DfaId source = dfa_.filter(layers_[ply].active, task.cube);
          if (profiling) {
            Profile::get().line("step,ply=%zu,job=%zu,move=%d,k=-2,kind=source,created=%llu,root=%llu,new_reach=0,"
                                "filter_bytes=%zu,seconds=%.3f", ply, job, task.move,
                                static_cast<unsigned long long>(dfa_.created_here() - created), static_cast<unsigned long long>(source),
                                dfa_.filter_bytes_here(), Profile::get().seconds() - started);
          }
          result = image(source, task.move, turn(ply));
          }
          if (profiling) {
            Profile::get().line("job,ply=%zu,wave=%ld,job=%zu,move=%d,created=%llu,root=%llu,new_reach=%llu,"
                                "reach=%llu,filter_bytes=%zu,seconds=%.3f", ply, wave_index, job, task.move,
                                static_cast<unsigned long long>(dfa_.created_here() - created), static_cast<unsigned long long>(result),
                                static_cast<unsigned long long>(Profile::get().reach() ? new_reachable(result, wave_base_) : 0),
                                static_cast<unsigned long long>(Profile::get().reach() ? dfa_.reachable_nodes(result) : 0),
                                dfa_.filter_bytes_here(), Profile::get().seconds() - started);
          }
          return std::vector<DfaId>{result};
        },
        [&](std::size_t job, const std::vector<DfaId>& roots) { wave[job] = roots[0]; },
        [&]() {
          census_line("pre_union", static_cast<long>(ply), wave_index);
          const std::uint64_t created_before = Profile::get().on() ? dfa_.census(false).created : 0;
          const std::size_t slots_before = dfa_.size();
          const auto union_started = std::chrono::steady_clock::now();
          wave.push_back(next);
          next = parallel_union(wave);
          wave.assign(tasks.size(), 0);
          const double this_union = std::chrono::duration<double>(std::chrono::steady_clock::now() - union_started).count();
          union_seconds += this_union;
          pool_before_gc = std::max(pool_before_gc, dfa_.size());
          if (Profile::get().on()) {
            Profile::get().line("union,ply=%zu,wave=%ld,created=%llu,slots_before=%zu,slots_after=%zu,root=%llu,"
                                "new_reach=%llu,reach=%llu,seconds=%.3f", ply, wave_index,
                                static_cast<unsigned long long>(dfa_.census(false).created - created_before),
                                slots_before, dfa_.size(), static_cast<unsigned long long>(next),
                                static_cast<unsigned long long>(Profile::get().reach() ? new_reachable(next, wave_base_) : 0),
                                static_cast<unsigned long long>(Profile::get().reach() ? dfa_.reachable_nodes(next) : 0),
                                this_union);
          }
          census_line("pre_gc", static_cast<long>(ply), wave_index);
          const auto gc_started = std::chrono::steady_clock::now();
          const std::size_t gc_before = dfa_.size();
          collect({&next});
          const double this_gc = std::chrono::duration<double>(std::chrono::steady_clock::now() - gc_started).count();
          gc_seconds += this_gc;
          if (Profile::get().on()) {
            Profile::get().line("gc,ply=%zu,wave=%ld,before=%zu,after=%zu,seconds=%.3f", ply, wave_index,
                                gc_before, dfa_.size(), this_gc);
          }
          census_line("post_gc", static_cast<long>(ply), wave_index);
          wave_base_ = dfa_.size();
          ++wave_index;
        });
    print_parallel_stats("forward", ply, stats);
    std::cout << "  union=" << union_seconds << " s, GC=" << gc_seconds
              << " s, transient_pool_nodes=" << pool_before_gc << '\n';
    return next;
  }

  void parallel_retrograde(std::size_t ply) {
    const auto tasks = placement_tasks(ply);
    DfaId to_loss = 0, to_nonwin = 0;
    DfaId nonwin = layer_union(layers_[ply + 1].loss, layers_[ply + 1].draw);
    collect({&nonwin});
    // Level-by-level preimages for large layers (TWIXT_PREIMAGE=level|memo forces one).
    static const std::string preimage_mode = [] { const char* v = std::getenv("TWIXT_PREIMAGE"); return v ? std::string(v) : std::string(); }();
    level_preimage_ = preimage_mode == "level" || (preimage_mode != "memo" && dfa_.size() >= 50'000'000);
    std::vector<DfaId> loss_wave(tasks.size()), nonwin_wave(tasks.size());
    const auto stats = twixt_dfa::parallel_jobs(dfa_, process_options(tasks.size()), tasks.size(), 2,
        [&](std::size_t job) {
          const auto& task = tasks[job];
          if (use_fused_preimage() && (task.cube[cell_level_[task.move]] & 1U) != 0) {
            // The fused preimage already lies inside this peg cube (its moved
            // cell is empty and allowed), so intersecting with the unfiltered
            // active layer is exact and avoids the filter's dense scratch.
            if (level_preimage_) {
              const DfaId active = layers_[ply].active;
              return std::vector<DfaId>{
                  level_preimage_within(layers_[ply + 1].loss, task.move, turn(ply), &task.cube, active),
                  level_preimage_within(nonwin, task.move, turn(ply), &task.cube, active)};
            }
            auto preimage_of = [&](DfaId target) { return fused_preimage(target, task.move, turn(ply), &task.cube); };
            DfaId loss = preimage_of(layers_[ply + 1].loss);
            loss = dfa_.intersect(loss, layers_[ply].active);
            DfaId other = preimage_of(nonwin);
            other = dfa_.intersect(other, layers_[ply].active);
            return std::vector<DfaId>{loss, other};
          }
          const DfaId active = dfa_.filter(layers_[ply].active, task.cube);
          if (active == 0) return std::vector<DfaId>{0, 0};
          SymbolWord child_cube = task.cube;
          child_cube[cell_level_[task.move]] = static_cast<std::uint8_t>(1U << color(turn(ply)));
          const DfaId target_loss = dfa_.filter(layers_[ply + 1].loss, child_cube);
          DfaId loss = preimage(target_loss, task.move, turn(ply));
          loss = dfa_.intersect(loss, active);
          const DfaId target_nonwin = dfa_.filter(nonwin, child_cube);
          DfaId other = preimage(target_nonwin, task.move, turn(ply));
          other = dfa_.intersect(other, active);
          return std::vector<DfaId>{loss, other};
        },
        [&](std::size_t job, const std::vector<DfaId>& roots) {
          loss_wave[job] = roots[0];
          nonwin_wave[job] = roots[1];
        },
        [&]() {
          loss_wave.push_back(to_loss); nonwin_wave.push_back(to_nonwin);
          std::vector<DfaId*> other_roots{&nonwin};
          for (auto& root : nonwin_wave) other_roots.push_back(&root);
          to_loss = parallel_union(loss_wave, other_roots);
          to_nonwin = parallel_union(nonwin_wave, {&nonwin, &to_loss});
          loss_wave.assign(tasks.size(), 0); nonwin_wave.assign(tasks.size(), 0);
          collect({&to_loss, &to_nonwin, &nonwin});
        });
    print_parallel_stats("retrograde", ply, stats);
    layers_[ply].win = to_loss;
    layers_[ply].loss = layer_union(layers_[ply].loss, layer_difference(layers_[ply].active, to_nonwin));
    layers_[ply].draw = layer_difference(layers_[ply].reachable, layer_union(layers_[ply].win, layers_[ply].loss));
  }

  bool forward() {
    if (layers_.empty()) add_layer(dfa_.singleton(encode(root_)));
    std::cout << "Symbolic forward generation (no symmetry; dead-draw descendants retained)\n"
              << "ply,positions,dfa_states,active_positions,pool_nodes,seconds\n";
    // A layer resumed from the store had its census line printed by the step that made it.
    const std::size_t resumed = store_ && store_->has(LayerStore::kForward, layers_.size() - 1)
                                    ? layers_.size() - 1 : std::numeric_limits<std::size_t>::max();
    for (std::size_t ply = layers_.size() - 1;; ++ply) {
      collect();
      if (store_) store_forward(ply);
      else if (checkpoint_hook_) checkpoint_hook_(0, 0);
      census_line("ply_start", static_cast<long>(ply), -1);
      const auto& layer = layers_[ply];
      if (ply == resumed) {
        std::cout << "# ply " << ply << " resumed from the layer store\n";
      } else {
        const LayerCensus c = layer_census({layer.reachable, layer.active});
        std::cout << ply << ',' << c.words[0] << ',' << c.nodes[0] << ',' << c.words[1] << ',' << dfa_.size()
                  << ',' << std::fixed << std::setprecision(3) << elapsed() << std::endl;
      }
      if (store_) {
        // Everything is on disk; generating the next layer needs active only.
        for (std::size_t k = 0; k < ply; ++k) layers_[k] = SymbolicLayer{};
        layers_[ply].reachable = layers_[ply].terminal_loss = layers_[ply].terminal_draw = 0;
      }
      if (layers_[ply].active == 0) return true;
      if (options_.symbolic_ply >= 0 && ply >= static_cast<std::size_t>(options_.symbolic_ply)) {
        return false;
      }
      DfaId next = 0;
      if (use_workers(ply)) {
        next = parallel_forward(ply);
      } else {
        for (int cell = 0; cell < game_.cell_count(); ++cell) {
          if (!game_.legal_cell(cell, turn(ply))) continue;
          // collect changes all root IDs; access the source again each time.
          const DfaId child = use_fused_image() ? fused_image(layers_[ply].active, cell, turn(ply)) :
                                                  image(layers_[ply].active, cell, turn(ply));
          next = dfa_.unite(next, child);
          maybe_collect({&next});
        }
      }
      if (next == 0) throw std::logic_error("active language has no successors");
      add_layer(next);
    }
  }

  // Write every in-memory forward layer up to ply that is not stored yet,
  // then commit the manifest.
  void store_forward(std::size_t ply) {
    for (std::size_t k = 0; k <= ply; ++k) {
      if (store_->has(LayerStore::kForward, k)) continue;
      const auto& l = layers_[k];
      if (l.reachable == 0) throw std::logic_error("forward layer " + std::to_string(k) + " is neither stored nor live");
      store_->write(dfa_, LayerStore::kForward, k, {l.reachable, l.terminal_loss, l.terminal_draw, l.active});
    }
    store_->phase = 0;
    store_->cursor = 0;
    store_->layer_count = static_cast<unsigned>(ply + 1);
    store_->save_manifest();
  }

  void load_forward(std::size_t ply) {
    auto& l = layers_[ply];
    if (l.reachable != 0) return;
    const auto roots = store_->read(dfa_, LayerStore::kForward, ply);
    l.reachable = roots[0]; l.terminal_loss = roots[1]; l.terminal_draw = roots[2]; l.active = roots[3];
  }

  void load_result(std::size_t ply) {
    auto& l = layers_[ply];
    if (l.win != 0 || l.loss != 0 || l.draw != 0) return;
    const auto roots = store_->read(dfa_, LayerStore::kResult, ply);
    l.win = roots[0]; l.loss = roots[1]; l.draw = roots[2];
  }

  // Load every stored layer into the manager (comparison, merging, probing
  // of small boards); results take precedence over forward languages.
  void materialize_layers() {
    if (!store_) return;
    layers_.resize(std::max<std::size_t>(layers_.size(), store_->layer_count));
    for (std::size_t ply = 0; ply < layers_.size(); ++ply) {
      if (store_->has(LayerStore::kResult, ply)) load_result(ply);
      else if (store_->has(LayerStore::kForward, ply)) load_forward(ply);
    }
  }

  // W/L/D of one ply from W/L/D of ply + 1 and this ply's forward languages.
  void solve_ply(std::size_t ply) {
    layers_[ply].loss = layers_[ply].terminal_loss;
    layers_[ply].draw = layers_[ply].terminal_draw;
    if (layers_[ply].active == 0) return;
    if (ply + 1 == layers_.size()) throw std::logic_error("incomplete final symbolic layer");
    if (use_workers(ply)) {
      parallel_retrograde(ply);
      return;
    }
    DfaId to_loss = 0, to_nonwin = 0;
    DfaId nonwin = dfa_.unite(layers_[ply + 1].loss, layers_[ply + 1].draw);
    for (int cell = 0; cell < game_.cell_count(); ++cell) {
      if (!game_.legal_cell(cell, turn(ply))) continue;
      DfaId parents = use_fused_preimage() ? fused_preimage(layers_[ply + 1].loss, cell, turn(ply)) :
                                             preimage(layers_[ply + 1].loss, cell, turn(ply));
      parents = dfa_.intersect(parents, layers_[ply].active);
      to_loss = dfa_.unite(to_loss, parents);
      parents = use_fused_preimage() ? fused_preimage(nonwin, cell, turn(ply)) :
                                       preimage(nonwin, cell, turn(ply));
      parents = dfa_.intersect(parents, layers_[ply].active);
      to_nonwin = dfa_.unite(to_nonwin, parents);
      maybe_collect({&to_loss, &to_nonwin, &nonwin});
    }
    layers_[ply].win = to_loss;
    layers_[ply].loss = dfa_.unite(layers_[ply].loss, dfa_.difference(layers_[ply].active, to_nonwin));
    layers_[ply].draw = dfa_.difference(layers_[ply].reachable, dfa_.unite(layers_[ply].win, layers_[ply].loss));
  }

  // Inverse of encode().
  State decode(const SymbolWord& word) const {
    State state;
    for (std::size_t level = 0; level < variables_.size(); ++level) {
      const auto v = variables_[level];
      const unsigned symbol = word[level];
      if (v.index < 0) {
        state.turn = static_cast<Player>(symbol);
      } else if (v.link) {
        if (symbol == 1) state.links[v.index / 64] |= 1ULL << (v.index % 64);
      } else if (symbol == 1) {
        state.white |= 1ULL << v.index;
      } else if (symbol == 2) {
        state.black |= 1ULL << v.index;
      }
    }
    return state;
  }

  // Word counts of every node reachable from `roots`, indexed by the node's
  // rank among them (8 bytes per reachable node, not per pool slot).
  struct WordCounts {
    std::unique_ptr<twixt_dfa::IdBitmap> bits;
    std::unique_ptr<twixt_dfa::IdRank> rank;
    std::vector<std::uint64_t> value;
    std::uint64_t of(DfaId id) const { return id < 2 ? id : value[rank->rank(id)]; }
  };
  WordCounts word_counts(const std::vector<DfaId>& roots) const {
    const unsigned threads = level_threads();
    const auto& nodes = dfa_.nodes();
    constexpr std::size_t kChunk = 1 << 16;
    bool accept = false;
    const auto levels = mark_levels(roots, accept);
    WordCounts counts;
    counts.bits = std::make_unique<twixt_dfa::IdBitmap>(dfa_.base(), dfa_.size() - dfa_.base());
    for (const auto& level : levels) {
      parallel_items(threads, (level.size() + kChunk - 1) / kChunk, [&](std::size_t c) {
        const std::size_t end = std::min<std::size_t>(level.size(), (c + 1) * kChunk);
        for (std::size_t i = c * kChunk; i < end; ++i) counts.bits->set_atomic(level[i]);
      });
    }
    counts.rank = std::make_unique<twixt_dfa::IdRank>(*counts.bits);
    counts.value.assign(counts.rank->count(), 0);
    std::atomic<bool> overflow{false};
    for (std::size_t level = levels.size(); level-- > 0;) {
      const std::vector<DfaId>& here = levels[level];
      parallel_items(threads, (here.size() + kChunk - 1) / kChunk, [&](std::size_t c) {
        const std::size_t end = std::min<std::size_t>(here.size(), (c + 1) * kChunk);
        for (std::size_t i = c * kChunk; i < end; ++i) {
          std::uint64_t sum = 0;
          for (DfaId child : nodes[here[i]].next) {
            const std::uint64_t n = counts.of(child);
            if (n > std::numeric_limits<std::uint64_t>::max() - sum) overflow.store(true);
            sum += n;
          }
          counts.value[counts.rank->rank(here[i])] = sum;
        }
      });
      if (overflow.load()) throw std::overflow_error("represented position count exceeds uint64");
    }
    return counts;
  }

  // `n` words drawn uniformly (with replacement) from the language of `root`.
  std::vector<SymbolWord> sample_words(DfaId root, std::size_t n, std::uint64_t seed) const {
    std::vector<SymbolWord> words;
    if (root < 2) return words;
    const WordCounts counts = word_counts({root});
    auto count_of = [&](DfaId id) -> std::uint64_t { return counts.of(id); };
    std::uint64_t random = seed;
    auto below = [&](std::uint64_t bound) {  // uniform enough in [0, bound)
      random += 0x9e3779b97f4a7c15ULL;
      const std::uint64_t x = twixt_dfa::hash64(random);
      return x % bound;  // bias below bound / 2^64
    };
    for (std::size_t i = 0; i < n; ++i) {
      SymbolWord word(dfa_.length(), 0);
      for (DfaId id = root; id >= 2;) {
        const twixt_dfa::Node node = dfa_.node(id);
        std::uint64_t r = below(count_of(id));
        unsigned chosen = 3;
        for (unsigned c = 0; c < 3; ++c) {
          const std::uint64_t k = count_of(node.next[c]);
          if (r < k) { chosen = c; break; }
          r -= k;
        }
        if (chosen == 3) throw std::logic_error("sample: count walk fell off the language");
        word[node.level] = static_cast<std::uint8_t>(chosen);
        id = node.next[chosen];
      }
      words.push_back(std::move(word));
    }
    return words;
  }

  // Independent spot check of a solved ply: positions drawn uniformly from the
  // layer get a W/L/D label from the explicit game rules (move generation,
  // links, connection test) and the stored labels of their successors; it
  // must equal the label the symbolic retrograde stored.
  void check_layer(std::size_t ply, std::size_t samples) const {
    const auto& layer = layers_[ply];
    const bool has_next = ply + 1 < layers_.size();
    const auto words = sample_words(layer.reachable, samples, 0x5eedULL * (ply + 1));
    std::array<std::size_t, 3> tally{};
    auto label = [&](const SymbolicLayer& l, const SymbolWord& w, const char* what) {
      const bool in_win = dfa_.contains(l.win, w), in_loss = dfa_.contains(l.loss, w), in_draw = dfa_.contains(l.draw, w);
      if (int(in_win) + int(in_loss) + int(in_draw) != 1) {
        throw std::logic_error(std::string("sample check: ") + what + " is not in exactly one of W/L/D");
      }
      return in_win ? 0 : in_loss ? 1 : 2;
    };
    for (const SymbolWord& w : words) {
      const State state = decode(w);
      if (encode(state) != w) throw std::logic_error("sample check: decode does not invert encode");
      const int stored = label(layer, w, "a sampled position");
      int expected = 2;
      if (game_.has_won(state, other(state.turn))) {
        expected = 1;
      } else {
        const auto moves = game_.legal_moves(state);
        if (!moves.empty()) {
          if (!has_next) throw std::logic_error("sample check: a position with moves in the last layer");
          bool any_loss = false, all_win = true;
          for (int move : moves) {
            const int successor = label(layers_[ply + 1], encode(game_.play(state, move)), "a successor");
            any_loss = any_loss || successor == 1;
            all_win = all_win && successor == 0;
          }
          expected = any_loss ? 0 : all_win ? 1 : 2;
        }
      }
      if (stored != expected) {
        throw std::logic_error("sample check: ply " + std::to_string(ply) + " position labelled " + "WLD"[stored] +
                               ", the rules give " + "WLD"[expected]);
      }
      ++tally[stored];
    }
    std::cout << "# sample check ply " << ply << ": " << words.size() << " positions consistent (W " << tally[0]
              << ", L " << tally[1] << ", D " << tally[2] << ")" << std::endl;
  }

  // Nodes reachable from `roots`, as per-level lists (level-synchronous
  // parallel marking); `accept` reports whether the accepting terminal is a
  // child.  The basis of the parallel census below.
  std::vector<std::vector<DfaId>> mark_levels(const std::vector<DfaId>& roots, bool& accept) const {
    const unsigned threads = level_threads();
    const auto& nodes = dfa_.nodes();
    const unsigned length = dfa_.length();
    constexpr std::size_t kChunk = 1 << 16;
    twixt_dfa::IdBitmap bits(dfa_.base(), dfa_.size() - dfa_.base());
    std::vector<std::vector<DfaId>> levels(length);
    for (DfaId r : roots) {
      if (r >= 2 && bits.set(r)) levels.at(nodes.level(r)).push_back(r);
    }
    std::atomic<bool> saw_accept{false};
    for (unsigned level = 0; level < length; ++level) {
      const std::vector<DfaId>& here = levels[level];
      const std::size_t chunks = (here.size() + kChunk - 1) / kChunk;
      std::vector<std::vector<DfaId>> found(chunks);
      parallel_items(threads, chunks, [&](std::size_t c) {
        const std::size_t end = std::min<std::size_t>(here.size(), (c + 1) * kChunk);
        bool one = false;
        for (std::size_t i = c * kChunk; i < end; ++i) {
          for (DfaId child : nodes[here[i]].next) {
            if (child == 1) one = true;
            else if (child >= 2 && bits.set_atomic(child)) found[c].push_back(child);
          }
        }
        if (one) saw_accept.store(true, std::memory_order_relaxed);
      });
      for (auto& f : found) {
        if (f.empty()) continue;
        if (level + 1 == length) throw std::logic_error("census: transition beyond the last level");
        levels[level + 1].insert(levels[level + 1].end(), f.begin(), f.end());
        std::vector<DfaId>().swap(f);
      }
    }
    accept = saw_accept.load();
    return levels;
  }
  // reachable_nodes(), in parallel.
  std::uint64_t layer_nodes(DfaId root) const {
    if (root < 2) return root;
    bool accept = false;
    std::uint64_t total = 0;
    for (const auto& level : mark_levels({root}, accept)) total += level.size();
    return total + (accept ? 1 : 0);
  }
  // count() of each root and layer_nodes() of each root, in parallel.
  struct LayerCensus { std::vector<std::uint64_t> words, nodes; };
  LayerCensus layer_census(const std::vector<DfaId>& roots) const {
    LayerCensus census;
    for (DfaId root : roots) census.nodes.push_back(layer_nodes(root));
    const WordCounts counts = word_counts(roots);
    for (DfaId root : roots) census.words.push_back(counts.of(root));
    return census;
  }

  void print_result_line(std::size_t ply) {
    const auto& layer = layers_[ply];
    const LayerCensus c = layer_census({layer.win, layer.draw, layer.loss});
    std::cout << ply << ',' << c.words[0] << ',' << c.words[1] << ',' << c.words[2] << ','
              << c.nodes[0] + c.nodes[1] + c.nodes[2] << ',' << dfa_.size() << ',' << std::fixed
              << std::setprecision(3) << elapsed() << std::endl;
  }

  // Retrograde with per-layer storage: only layer ply's forward languages and
  // layer ply+1's W/L/D are live while solving ply.  `stop_after` (testing)
  // stops once that ply is solved and committed.
  void retrograde_stored(std::size_t remaining, long stop_after) {
    layers_.resize(std::max<std::size_t>(layers_.size(), store_->layer_count));
    // A monolithic checkpoint may hold layers that are not stored yet.
    for (std::size_t ply = 0; ply < layers_.size(); ++ply) {
      const auto& l = layers_[ply];
      if (ply >= remaining && (l.win != 0 || l.loss != 0 || l.draw != 0) && !store_->has(LayerStore::kResult, ply)) {
        store_->write(dfa_, LayerStore::kResult, ply, {l.win, l.loss, l.draw});
      }
      if (ply < remaining && l.reachable != 0 && !store_->has(LayerStore::kForward, ply)) {
        store_->write(dfa_, LayerStore::kForward, ply, {l.reachable, l.terminal_loss, l.terminal_draw, l.active});
      }
    }
    store_->layer_count = static_cast<unsigned>(layers_.size());
    for (std::size_t ply = 0; ply < layers_.size(); ++ply) {
      if (ply + 1 != remaining) layers_[ply].reachable = layers_[ply].terminal_loss = layers_[ply].terminal_draw =
          layers_[ply].active = 0;
      if (ply != remaining) layers_[ply].win = layers_[ply].loss = layers_[ply].draw = 0;
    }
    for (std::size_t ply = remaining; ply-- > 0;) {
      load_forward(ply);
      if (ply + 1 < layers_.size()) load_result(ply + 1);
      solve_ply(ply);
      if (options_.sample_check) check_layer(ply, options_.sample_check);  // before anything is committed
      layers_[ply].reachable = layers_[ply].active = 0;
      layers_[ply].terminal_loss = layers_[ply].terminal_draw = 0;
      store_->write(dfa_, LayerStore::kResult, ply, {layers_[ply].win, layers_[ply].loss, layers_[ply].draw});
      if (ply + 1 < layers_.size()) layers_[ply + 1] = SymbolicLayer{};
      collect();
      store_->phase = ply == 0 ? 2 : 1;
      store_->cursor = static_cast<unsigned>(ply);
      store_->save_manifest();
      print_result_line(ply);
      if (stop_after >= 0 && ply == static_cast<std::size_t>(stop_after)) {
        throw twixt_dfa::Limit("retrograde stopped after ply " + std::to_string(ply) + " as requested");
      }
    }
  }

  void retrograde(std::size_t remaining = std::numeric_limits<std::size_t>::max()) {
    std::cout << "Symbolic retrograde\nply,win,draw,loss,wdl_dfa_states,pool_nodes,seconds\n";
    if (remaining == std::numeric_limits<std::size_t>::max()) remaining = layers_.size();
    if (store_) {
      retrograde_stored(remaining, options_.retrograde_stop);
      return;
    }
    // Solved W/L/D languages partition reachability.  Keeping their separate
    // R and active/terminal languages would retain millions of redundant
    // nodes.  This also upgrades older checkpoints that retained those roots.
    for (std::size_t ply = remaining; ply < layers_.size(); ++ply) {
      layers_[ply].reachable = layers_[ply].active = 0;
      layers_[ply].terminal_loss = layers_[ply].terminal_draw = 0;
    }
    for (std::size_t ply = remaining; ply-- > 0;) {
      solve_ply(ply);
      layers_[ply].reachable = layers_[ply].active = 0;
      layers_[ply].terminal_loss = layers_[ply].terminal_draw = 0;
      collect();
      if (checkpoint_hook_) checkpoint_hook_(1, static_cast<unsigned>(ply));
      print_result_line(ply);
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
  std::unique_ptr<LayerStore> store_;
  std::size_t wave_base_ = 0;
  std::size_t last_won_memo_entries_ = 0, last_won_memo_buckets_ = 0, last_won_frontiers_ = 0;
  std::size_t last_won_widest_ = 0;
  bool level_image_ = false;     // set per forward ply before its jobs start
  bool level_preimage_ = false;  // set per retrograde ply before its jobs start
  std::atomic<std::uint64_t> spill_counter_{0};  // spill files may be created by concurrent jobs
  std::uint64_t last_union_tuples_ = 0, last_union_widest_ = 0;
  std::array<double, 3> last_union_phases_{};  // expand, flatten, replay seconds

 public:
  std::uint64_t level_union_runs_ = 0;  // unions that took the level path (self-test)

 private:
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
  // The pool is compact after collection: nonterminals are [base, size).
  const DfaId base = solution.manager().base();
  const std::uint64_t local_count = solution.manager().size() - base + 2;
  // Version 2 keeps 32-bit node references (readable by older builds);
  // version 3 stores 64-bit roots and 5-byte child references.
  const bool wide = local_count > 0xffffffffULL;
  auto local = [&](DfaId id) -> std::uint64_t { return id < 2 ? id : id - base + 2; };
  auto put_root = [&](DfaId id) { if (wide) write_u64(out, local(id)); else write_u32(out, static_cast<std::uint32_t>(local(id))); };
  out.write(checkpoint ? "TWXCKP01" : "TWXDFA01", 8);
  write_u32(out, wide ? 3 : 2);  // sparse arcs; version 1 stored level + three child IDs
  write_u32(out, static_cast<std::uint32_t>(game.width()));
  write_u32(out, static_cast<std::uint32_t>(game.height()));
  write_u32(out, 1);  // spatial variable order
  write_u32(out, solution.manager().length());
  write_u64(out, local_count);
  write_u32(out, static_cast<std::uint32_t>(solution.layers().size()));
  const Key root_key = game.canonical_key(solution.root_state(), false);
  for (auto word : root_key.words) write_u64(out, word);
  if (checkpoint) {
    write_u32(out, phase);
    write_u32(out, remaining);
  }
  for (const auto& layer : solution.layers()) {
    put_root(layer.reachable);
    put_root(layer.win);
    put_root(layer.loss);
    put_root(layer.draw);
    if (checkpoint) {
      put_root(layer.terminal_loss);
      put_root(layer.terminal_draw);
      put_root(layer.active);
    }
  }
  std::uint64_t node_bytes = 0;
  for (std::size_t id = base; id < solution.manager().size(); ++id) {
    const twixt_dfa::Node node = solution.manager().nodes()[id];
    unsigned mask = 0;
    for (unsigned c = 0; c < 3; ++c) if (node.next[c] != 0) mask |= 1U << c;
    if (mask == 0) throw std::logic_error("archive writer needs a compacted DFA pool");
    out.put(static_cast<char>(mask));
    ++node_bytes;
    for (DfaId child : node.next) {
      if (child == 0) continue;
      const std::uint64_t value = local(child);
      if (wide) { for (int b = 0; b < 5; ++b) out.put(static_cast<char>((value >> (8 * b)) & 0xffU)); node_bytes += 5; }
      else { write_u32(out, static_cast<std::uint32_t>(value)); node_bytes += 4; }
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
  std::cout << "DFA solution written to " << path << " ("
            << solution.manager().size() - solution.manager().base() + 2
            << " shared nodes, " << node_bytes << " sparse node bytes).\n";
}

class SymbolicDatabase {
 public:
  explicit SymbolicDatabase(const std::string& path, const Options* run_options = nullptr) {
    if (LayerStore::is_manifest(path)) {
      open_manifest(path, run_options);
      return;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open DFA solution: " + path);
    char magic[8]{};
    in.read(magic, 8);
    checkpoint_ = std::string(magic, 8) == "TWXCKP01";
    const unsigned version = read_u32(in);
    if ((!checkpoint_ && std::string(magic, 8) != "TWXDFA01") || version < 1 || version > 3) {
      throw std::runtime_error("not a supported TWXDFA01 solution");
    }
    const bool wide = version == 3;  // 64-bit roots, 5-byte child references
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
        node_count >= twixt_dfa::kMaxId ||
        layer_count == 0 || layer_count > kMaxCells + 1 || phase_ > 2 ||
        remaining_ > layer_count) {
      throw std::runtime_error("invalid DFA solution dimensions");
    }
    const auto records_begin = in.tellg();
    in.seekg(0, std::ios::end);
    const std::uint64_t file_size = static_cast<std::uint64_t>(in.tellg());
    const std::uint64_t data_begin = static_cast<std::uint64_t>(records_begin) +
                                    (checkpoint_ ? 7ULL : 4ULL) * (wide ? 8ULL : 4ULL) * layer_count;
    const std::uint64_t minimum = data_begin + (version == 1 ? 16ULL : wide ? 6ULL : 5ULL) * (node_count - 2);
    const std::uint64_t maximum = data_begin + (version == 1 ? 16ULL : wide ? 16ULL : 13ULL) * (node_count - 2);
    if (file_size < minimum || file_size > maximum) {
      throw std::runtime_error("DFA solution file size mismatch");
    }
    in.seekg(records_begin);
    solution_->layers().resize(layer_count);
    auto get_root = [&]() -> DfaId { return wide ? read_u64(in) : read_u32(in); };
    for (auto& layer : solution_->layers()) {
      layer.reachable = get_root();
      layer.win = get_root();
      layer.loss = get_root();
      layer.draw = get_root();
      if (checkpoint_) {
        layer.terminal_loss = get_root();
        layer.terminal_draw = get_root();
        layer.active = get_root();
      }
      for (DfaId root : {layer.reachable, layer.win, layer.loss, layer.draw,
                         layer.terminal_loss, layer.terminal_draw, layer.active}) {
        if (root >= node_count) throw std::runtime_error("invalid DFA root ID");
      }
    }
    // File IDs are local (nonterminals from 2); the manager numbers them
    // from its base, in the same order.
    const DfaId base = solution_->manager().base();
    auto to_manager = [&](std::uint64_t local) -> DfaId { return local < 2 ? local : base + (local - 2); };
    auto get_child = [&]() -> std::uint64_t {
      if (!wide) return read_u32(in);
      std::uint64_t value = 0;
      for (int b = 0; b < 5; ++b) {
        const int ch = in.get();
        if (ch == std::char_traits<char>::eof()) throw std::runtime_error("truncated DFA solution");
        value |= static_cast<std::uint64_t>(static_cast<unsigned char>(ch)) << (8 * b);
      }
      return value;
    };
    for (std::uint64_t id = 2; id < node_count; ++id) {
      unsigned level = 0;
      if (version == 1) level = read_u32(in);
      const unsigned mask = version == 1 ? 7 : static_cast<unsigned>(in.get());
      if (mask == 0 || mask > 7) throw std::runtime_error("invalid sparse DFA arc mask");
      twixt_dfa::Children children{};
      for (unsigned c = 0; c < 3; ++c) {
        if ((mask & (1U << c)) == 0) continue;
        const std::uint64_t child = get_child();
        if (child >= id || (version >= 2 && child == 0)) {
          throw std::runtime_error("invalid DFA topological order");
        }
        children[c] = to_manager(child);
        if (version >= 2) {
          const unsigned child_level = solution_->manager().node(children[c]).level;
          if (child_level == 0) throw std::runtime_error("invalid DFA transition level");
          level = child_level - 1;
        }
      }
      if (solution_->manager().intern(level, children) != to_manager(id)) {
        throw std::runtime_error("nonminimal or duplicate DFA node");
      }
    }
    for (auto& layer : solution_->layers()) {
      for (DfaId* root : {&layer.reachable, &layer.win, &layer.loss, &layer.draw,
                          &layer.terminal_loss, &layer.terminal_draw, &layer.active}) {
        *root = to_manager(*root);
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
  // A per-layer solution: load only what the next step needs.  Phase 0
  // resumes forward generation from the last layer, phase 1 resumes
  // retrograde from W/L/D(cursor), phase 2 is complete (probe on demand).
  void open_manifest(const std::string& path, const Options* run_options) {
    auto store = LayerStore::open(path);
    width_ = store->width();
    height_ = store->height();
    phase_ = store->phase;
    remaining_ = store->cursor;
    checkpoint_ = phase_ < 2;
    if (checkpoint_ && run_options == nullptr) {
      throw std::invalid_argument("an unfinished layer store requires --resume-dfa, not --probe-dfa");
    }
    game_ = std::make_unique<Twixt>(width_, height_);
    Options options;
    options.census_seconds = 0;
    options.dfa_max_nodes = 0;
    if (run_options) options = *run_options;
    Key root_key;
    for (std::size_t i = 0; i < root_key.words.size(); ++i) root_key.words[i] = store->root_key[i];
    solution_ = std::make_unique<SymbolicTwixt>(*game_, game_->unpack_key(root_key), options);
    store->set_threads(options.level_threads ? options.level_threads : std::max(1U, options.workers));
    if (store->layer_count == 0 || store->layer_count > kMaxCells + 1) {
      throw std::runtime_error("invalid layer count in manifest");
    }
    solution_->layers().resize(store->layer_count);
    if (phase_ == 0) {
      const std::size_t last = store->layer_count - 1;
      const auto roots = store->read(solution_->manager(), LayerStore::kForward, last);
      auto& l = solution_->layers()[last];
      l.reachable = roots[0]; l.terminal_loss = roots[1]; l.terminal_draw = roots[2]; l.active = roots[3];
    } else if (phase_ == 1) {
      const auto roots = store->read(solution_->manager(), LayerStore::kResult, remaining_);
      auto& l = solution_->layers()[remaining_];
      l.win = roots[0]; l.loss = roots[1]; l.draw = roots[2];
    }
    solution_->set_layer_store(std::move(store));
  }
  int width_ = 0, height_ = 0;
  bool checkpoint_ = false;
  unsigned phase_ = 2, remaining_ = 0;
  std::unique_ptr<Twixt> game_;
  std::unique_ptr<SymbolicTwixt> solution_;
};

void compare_symbolic(const Twixt& game, SymbolicTwixt& solution, const std::string& path) {
  Options read_options;
  read_options.census_seconds = 0;
  read_options.dfa_max_nodes = 0;
  SymbolicDatabase reference(path, &read_options);
  if (reference.width() != game.width() || reference.height() != game.height() ||
      reference.solution().layers().size() != solution.layers().size() ||
      !(game.canonical_key(reference.solution().root_state(), false) ==
        game.canonical_key(solution.root_state(), false))) {
    throw std::invalid_argument("DFA comparison has a different geometry, root, or layer count");
  }
  const auto& left = solution.manager();
  const auto& right = reference.solution().manager();
  twixt_dfa::FlatMap mapped;  // left nonterminal -> right node; sparse in the pool size
  std::vector<std::pair<DfaId, DfaId>> pending;
  const bool reachability = reference.phase() == 0;
  if (reference.phase() == 1) throw std::invalid_argument("compare a forward checkpoint or a completed solution");
  for (std::size_t ply = 0; ply < solution.layers().size(); ++ply) {
    const auto& a = solution.layers()[ply];
    const auto& b = reference.solution().layers()[ply];
    if (reachability) {
      if (a.reachable == 0) throw std::invalid_argument("forward checkpoint comparison needs reachable layers");
      pending.emplace_back(a.reachable, b.reachable);
      pending.emplace_back(a.terminal_loss, b.terminal_loss);
      pending.emplace_back(a.terminal_draw, b.terminal_draw);
      pending.emplace_back(a.active, b.active);
    } else {
      if (a.win == 0 && a.loss == 0 && a.draw == 0) {
        throw std::invalid_argument("completed solution comparison needs solved W/L/D layers");
      }
      pending.emplace_back(a.win, b.win); pending.emplace_back(a.loss, b.loss);
      pending.emplace_back(a.draw, b.draw);
    }
  }
  std::uint64_t checked = 0;
  while (!pending.empty()) {
    const auto [a, b] = pending.back(); pending.pop_back();
    if (a < 2 || b < 2) {
      if (a != b) throw std::logic_error("DFA W/L/D languages differ");
      continue;
    }
    if (DfaId known = 0; mapped.find(a, known)) {
      if (known != b) throw std::logic_error("DFA W/L/D languages differ");
      continue;
    }
    const twixt_dfa::Node left_node = left.node(a), right_node = right.node(b);
    if (left_node.level != right_node.level) throw std::logic_error("DFA W/L/D languages differ");
    mapped.insert(a, b);
    for (unsigned c = 0; c < 3; ++c) pending.emplace_back(left_node.next[c], right_node.next[c]);
    if (((++checked) & 0x3fffULL) == 0) solution.check_time();
  }
  std::cout << "Exact " << (reachability ? "reachable/terminal/active" : "W/L/D")
            << " language equivalence passed against " << path << " ("
            << checked << " shared DFA nodes checked).\n";
}

void probe_symbolic(const Twixt& game, const State& state,
                    SymbolicTwixt& solution, bool quiet) {
  const int ply = peg_count(state) - peg_count(solution.root_state());
  if (ply < 0) throw std::invalid_argument("position precedes DFA solution root");
  if (LayerStore* store = solution.layer_store()) {
    for (std::size_t k : {static_cast<std::size_t>(ply), static_cast<std::size_t>(ply) + 1}) {
      if (k < solution.layers().size() && store->has(LayerStore::kResult, k)) solution.load_result(k);
    }
  }
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
    if (!options.layer_dir.empty() && solution.layer_store() == nullptr) {
      auto store = std::make_unique<LayerStore>(options.layer_dir, game.width(), game.height(),
                                                solution.manager().length());
      if (std::filesystem::exists(store->manifest_path())) {
        throw std::invalid_argument("layer directory already holds a manifest; resume it with --resume-dfa " +
                                    store->manifest_path());
      }
      const Key key = game.canonical_key(solution.root_state(), false);
      for (std::size_t i = 0; i < key.words.size(); ++i) store->root_key[i] = key.words[i];
      solution.set_layer_store(std::move(store));
    }
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
    if (!options.compare_dfa.empty() || options.symbolic_verify || !options.dfa_out.empty()) {
      solution.materialize_layers();  // small boards: comparison needs every layer live
    }
    if (!options.compare_dfa.empty()) compare_symbolic(game, solution, options.compare_dfa);
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

// Fused and classic images must produce the identical canonical DFA for
// languages with shared structure, every legal move, and random peg cubes.

// Diagnostic for the variable-order question.  The side-to-move bit is the
// LAST variable, so equal peg/link suffixes of opposite turns can never share
// a node, and every image must rebuild every node.  Recanonicalize all nodes
// with the turn level collapsed: that is the graph a turn-first order would
// keep (plus one level-0 node per root).  Optionally image the last layer by
// every move and count the nodes each order would newly create.
void turn_sharing_stats(SymbolicTwixt& solution, bool images) {
  auto& dfa = solution.manager();
  if (dfa.base() != 2) throw std::invalid_argument("--turn-sharing-stats needs the default DFA ID base");
  const unsigned turn_level = dfa.length() - 1;
  std::vector<std::uint8_t> alphabet(dfa.alphabet().begin(), dfa.alphabet().end() - 1);
  twixt_dfa::Manager stripped(alphabet, 0);
  std::vector<DfaId> map{0, 1};
  auto extend = [&]() {
    for (std::size_t id = map.size(); id < dfa.size(); ++id) {
      const twixt_dfa::Node node = dfa.nodes()[id];
      if (node.next == twixt_dfa::Children{}) { map.push_back(0); continue; }  // unused slot
      if (node.level == turn_level) { map.push_back(1); continue; }
      twixt_dfa::Children children{};
      for (unsigned c = 0; c < 3; ++c) children[c] = map[node.next[c]];
      map.push_back(stripped.intern(node.level, children));
    }
  };
  extend();
  auto count_all = [](const twixt_dfa::Manager& m, const std::vector<DfaId>& roots) {
    std::vector<std::uint8_t> seen(m.size());
    std::vector<DfaId> stack;
    std::uint64_t count = 0;
    for (DfaId r : roots) if (r >= 2 && !seen[r]) { seen[r] = 1; stack.push_back(r); }
    while (!stack.empty()) {
      const DfaId id = stack.back(); stack.pop_back(); ++count;
      for (DfaId child : m.nodes()[id].next) if (child >= 2 && !seen[child]) { seen[child] = 1; stack.push_back(child); }
    }
    return count;
  };
  std::vector<DfaId> all_roots, all_stripped;
  std::cout << "ply,layer_nodes_turn_last,layer_nodes_turn_stripped\n";
  for (std::size_t ply = 0; ply < solution.layers().size(); ++ply) {
    const auto& layer = solution.layers()[ply];
    for (DfaId r : {layer.reachable, layer.terminal_loss, layer.terminal_draw, layer.active,
                    layer.win, layer.loss, layer.draw}) {
      all_roots.push_back(r);
      all_stripped.push_back(map[r]);
    }
    std::cout << ply << ',' << dfa.reachable_nodes(layer.reachable) << ','
              << stripped.reachable_nodes(map[layer.reachable]) << '\n';
  }
  std::cout << "retained nodes: turn-last " << count_all(dfa, all_roots)
            << ", turn-stripped " << count_all(stripped, all_stripped)
            << " (+" << 2 * solution.layers().size() << " turn roots at most)\n";
  if (!images) return;
  const std::size_t ply = solution.layers().size() - 1;
  const Player mover = solution.turn(ply);
  std::uint64_t total_last = 0, total_first = 0;
  for (int move = 0; move < 64; ++move) {
    if (!solution.legal_move(move, mover)) continue;
    const std::size_t before = dfa.size(), stripped_before = stripped.size();
    const DfaId image = solution.fused_image(solution.layers()[ply].active, move, mover);
    extend();
    const std::uint64_t last = dfa.size() - before, first = stripped.size() - stripped_before;
    total_last += last;
    total_first += first;
    std::cout << "image ply " << ply << " move " << move << ": new nodes turn-last " << last
              << ", turn-first " << first << ", image nodes " << dfa.reachable_nodes(image) << '\n';
  }
  std::cout << "all images of ply " << ply << ": new nodes turn-last " << total_last
            << ", turn-first " << total_first << '\n';
}

void fused_image_self_test() {
  std::uint64_t random = 0x9e3779b97f4a7c15ULL;
  std::uint64_t checks = 0;
  for (const auto& [width, height] : std::vector<std::pair<int, int>>{{5, 5}, {5, 6}, {6, 6}}) {
    Twixt game(width, height);
    Options options;
    options.census_seconds = 0;
    options.dfa_max_nodes = 0;
    SymbolicTwixt symbolic(game, State{}, options);
    auto& dfa = symbolic.manager();
    for (int ply = 1; ply <= std::min(20, game.cell_count() - 6); ply += 3) {
      for (int trial = 0; trial < 3; ++trial) {
        DfaId language = 0;
        Player turn = Player::White;
        std::vector<State> parents;
        for (int game_index = 0; game_index < 60; ++game_index) {
          State state;
          bool ok = true;
          for (int i = 0; i < ply; ++i) {
            if (terminal_outcome(game, state, false).has_value()) { ok = false; break; }
            const auto moves = game.legal_moves(state);
            random = mix64(random);
            state = game.play(state, moves[random % moves.size()]);
          }
          if (!ok || terminal_outcome(game, state, false).has_value()) continue;
          turn = state.turn;
          parents.push_back(state);
          language = dfa.unite(language, dfa.singleton(symbolic.encode(state)));
        }
        if (language == 0) continue;
        for (int move = 0; move < game.cell_count(); ++move) {
          if (!game.legal_cell(move, turn)) continue;
          SymbolWord cube = dfa.unrestricted();
          for (int cell = 0; cell < game.cell_count(); ++cell) {
            random = mix64(random);
            if (!game.valid_cell(cell) || random % 4 != 0) continue;
            cube[symbolic.cell_level(cell)] = static_cast<std::uint8_t>(1U + (random >> 8) % 7);
          }
          const DfaId child = symbolic.image(language, move, turn);
          if (child != symbolic.fused_image(language, move, turn) ||
              child != symbolic.level_fused_image(language, move, turn)) {
            throw std::logic_error("fused or level image differs from classic image");
          }
          {
            const DfaId cube_image = symbolic.image(dfa.filter(language, cube), move, turn);
            if (cube_image != symbolic.fused_image(language, move, turn, &cube) ||
                cube_image != symbolic.level_fused_image(language, move, turn, &cube)) {
              throw std::logic_error("fused or level cube image differs from classic image");
            }
          }
          // Arbitrary targets: true children plus children whose incident,
          // crossing, or arbitrary link bits were toggled (invalid updates).
          DfaId target = child;
          for (const State& parent : parents) {
            if (game.occupied(parent, move)) continue;
            State corrupted = game.play(parent, move);
            random = mix64(random);
            const auto& incident = game.incident_edges(move);
            const int edge = random % 3 == 0 ? static_cast<int>((random >> 8) % game.edge_count())
                                             : incident[(random >> 8) % incident.size()];
            corrupted.links[edge / 64] ^= 1ULL << (edge % 64);
            target = dfa.unite(target, dfa.singleton(symbolic.encode(corrupted)));
          }
          SymbolWord child_cube = cube;
          child_cube[symbolic.cell_level(move)] = static_cast<std::uint8_t>(1U << (turn == Player::White ? 1 : 2));
          for (const DfaId t : {child, target}) {
            const DfaId classic = symbolic.preimage(t, move, turn);
            if (classic != symbolic.fused_preimage(t, move, turn) ||
                classic != symbolic.level_fused_preimage(t, move, turn)) {
              throw std::logic_error("fused or level preimage differs from classic preimage");
            }
            const DfaId fused_cube = symbolic.fused_preimage(t, move, turn, &cube);
            if (fused_cube != symbolic.level_fused_preimage(t, move, turn, &cube)) {
              throw std::logic_error("level cube preimage differs from fused cube preimage");
            }
            for (const DfaId within : {language, dfa.filter(language, cube), child}) {
              if (symbolic.level_preimage_within(t, move, turn, &cube, within) != dfa.intersect(fused_cube, within) ||
                  symbolic.level_preimage_within(t, move, turn, nullptr, within) != dfa.intersect(classic, within)) {
                throw std::logic_error("restricted level preimage differs from preimage intersection");
              }
            }
            if (symbolic.preimage(dfa.filter(t, child_cube), move, turn) != fused_cube) {
              throw std::logic_error("fused cube preimage differs from classic preimage");
            }
            if ((cube[symbolic.cell_level(move)] & 1U) != 0 &&
                dfa.intersect(fused_cube, language) !=
                dfa.intersect(symbolic.preimage(dfa.filter(t, child_cube), move, turn), dfa.filter(language, cube))) {
              throw std::logic_error("unfiltered retrograde intersection differs");
            }
            checks += 2;
          }
          {
            std::vector<DfaId> parts{child, target, language, dfa.filter(language, cube)};
            const DfaId expected = symbolic.pairwise_union(parts);
            if (dfa.unite_many(parts) != expected || symbolic.level_union(parts, 1) != expected ||
                symbolic.level_union(parts, 5) != expected) {
              throw std::logic_error("n-ary or level union differs from pairwise union");
            }
            // Wider unions: random cube filters of the layer, as wave results overlap.
            std::vector<DfaId> many{child, target};
            for (int k = 0; k < 12; ++k) {
              SymbolWord random_cube = dfa.unrestricted();
              for (int cell = 0; cell < game.cell_count(); ++cell) {
                random = mix64(random);
                if (!game.valid_cell(cell) || random % 3 != 0) continue;
                random_cube[symbolic.cell_level(cell)] = static_cast<std::uint8_t>(1U + (random >> 8) % 7);
              }
              many.push_back(dfa.filter(language, random_cube));
            }
            for (std::size_t i = 0; i + 1 < many.size(); ++i) {
              const DfaId x = many[i], y = many[i + 1];
              if (symbolic.level_binary(SymbolicTwixt::LevelOp::Intersect, x, y, 3) != dfa.intersect(x, y) ||
                  symbolic.level_binary(SymbolicTwixt::LevelOp::Difference, x, y, 3) != dfa.difference(x, y) ||
                  symbolic.level_binary(SymbolicTwixt::LevelOp::Difference, y, x, 1) != dfa.difference(y, x)) {
                throw std::logic_error("level intersection or difference differs");
              }
            }
            {
              const auto census = symbolic.layer_census(many);
              for (std::size_t i = 0; i < many.size(); ++i) {
                if (census.words[i] != dfa.count(many[i]) || census.nodes[i] != dfa.reachable_nodes(many[i])) {
                  throw std::logic_error("parallel census differs from count/reachable_nodes");
                }
              }
            }
            const DfaId wide = symbolic.pairwise_union(many);
            if (dfa.unite_many(many) != wide || symbolic.level_union(many, 1) != wide ||
                symbolic.level_union(many, 7) != wide) {
              throw std::logic_error("wide n-ary or level union differs from pairwise union");
            }
            checks += 2;
          }
          for (Player player : {Player::White, Player::Black}) {
            const DfaId expected = symbolic.classic_won(child, player);
            if (expected != symbolic.compact_won(child, player) ||
                expected != symbolic.level_won(child, player, 1) ||
                expected != symbolic.level_won(child, player, 5)) {
              throw std::logic_error("compact or level connection recognizer differs");
            }
          }
          checks += 4;
        }
        symbolic.collect();
      }
    }
    if (symbolic.level_union_runs_ == 0) throw std::logic_error("self-test never reached the level union");
  }
  std::cout << "Fused image/preimage self-test: " << checks << " language checks matched.\n";
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
      if (symbolic.image(source, move, state.turn) != expected ||
          symbolic.fused_image(source, move, state.turn) != expected) {
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
