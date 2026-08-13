// Slice file I/O: see slice_file.h for the format description.

#include "slice_file.h"
#include <stdexcept>
#include <cstring>
#include <algorithm>

namespace pgs {

using std::string;
using std::vector;
using std::runtime_error;

static void die(const string& msg) { throw runtime_error(msg); }

static void xwrite(FILE* f, const void* data, uint64_t size, uint64_t off) {
  if (fseeko(f, (off_t)off, SEEK_SET) || fwrite(data, 1, size, f) != size)
    die("write failed");
}

static void xread(FILE* f, void* data, uint64_t size, uint64_t off) {
  if (fseeko(f, (off_t)off, SEEK_SET) || fread(data, 1, size, f) != size)
    die("read failed");
}

vector<uint8_t> compress_block(RawArray<const Vector<super_t,2>> data, int level) {
  const uLongf src = (uLongf)(64*data.size());
  vector<uint8_t> out(compressBound(src));
  uLongf dst = (uLongf)out.size();
  const int rc = compress2(out.data(), &dst, (const Bytef*)data.data(), src, level);
  if (rc != Z_OK) die("zlib compress2 failed");
  out.resize(dst);
  return out;
}

void uncompress_block(const uint8_t* src, size_t src_size, RawArray<Vector<super_t,2>> dst) {
  uLongf n = (uLongf)(64*dst.size());
  const int rc = uncompress((Bytef*)dst.data(), &n, src, (uLong)src_size);
  if (rc != Z_OK || n != 64*(uLongf)dst.size()) die("zlib uncompress failed");
}

slice_writer_t::slice_writer_t(const string& path, int slice,
                               RawArray<const section_t> sections, int level)
  : path(path), level(level) {
  memset(&header, 0, sizeof(header));
  memcpy(header.magic, slice_magic, 8);
  header.version = slice_version;
  header.slice = slice;
  header.section_count = sections.size();
  entries.resize(sections.size());
  for (int i=0;i<sections.size();i++) {
    entries[i].sig = sections[i].sig();
    const auto shape = sections[i].shape();
    for (int d=0;d<4;d++) entries[i].shape[d] = shape[d];
  }

  // Restart if a partial/complete file exists
  if (FILE* old = fopen(path.c_str(), "rb")) {
    slice_header_t h;
    if (fread(&h, sizeof(h), 1, old) == 1
        && !memcmp(h.magic, slice_magic, 8) && h.version == slice_version
        && h.slice == (uint32_t)slice && h.section_count == (uint64_t)sections.size()) {
      vector<section_entry_t> old_entries(sections.size());
      if (fread(old_entries.data(), 48, sections.size(), old) == sections.size()) {
        uint64_t end = 64 + 48*sections.size();
        uint64_t done = 0;
        for (auto& e : old_entries)
          if (e.flags & SECTION_COMPLETE) {
            if (e.sig != entries[done].sig || memcmp(e.shape, entries[done].shape, 16))
              die("restart section mismatch: file does not match section list");
            entries[done++] = e;
            end = std::max(end, e.payload_off + e.payload_len);
          } else break;  // sections are written in order; stop at first incomplete
        header.sections_written = done;
        fclose(old);
        f = fopen(path.c_str(), "r+b");
        if (!f) die("reopen failed");
        if (ftruncate(fileno(f), (off_t)end)) die("truncate failed");
        xwrite(f, &header, sizeof(header), 0);
        xwrite(f, entries.data(), 48*entries.size(), 64);
        return;
      }
    }
    fclose(old);
    f = fopen(path.c_str(), "w+b");
    if (!f) die("create failed");
  } else {
    f = fopen(path.c_str(), "w+b");
    if (!f) die("create failed");
  }
  xwrite(f, &header, sizeof(header), 0);
  xwrite(f, entries.data(), 48*entries.size(), 64);
}

slice_writer_t::~slice_writer_t() {
  if (f) fclose(f);
}

void slice_writer_t::write_section(int i, const section_t& section,
                                   RawArray<const Vector<super_t,2>> data) {
  if (section_done(i)) die("section already written");
  if ((uint64_t)i != header.sections_written) die("sections must be written in order");
  if ((uint64_t)data.size() != section.size()) die("section data size mismatch");

  const auto shape = section.shape();
  const auto blocks = section_blocks(section);
  const uint64_t nblocks = (uint64_t)blocks.product();

  // Compress all blocks
  vector<block_entry_t> index(nblocks);
  vector<vector<uint8_t>> compressed(nblocks);
  const auto shape_strides = strides(shape);
  uint64_t data_len = 0;
  for (uint64_t b=0;b<nblocks;b++) {
    const auto b4 = Vector<int,4>((int)(b/(blocks[1]*(uint64_t)blocks[2]*blocks[3])%blocks[0]),
                                  (int)(b/(blocks[2]*(uint64_t)blocks[3])%blocks[1]),
                                  (int)(b/blocks[3]%blocks[2]),
                                  (int)(b%blocks[3]));
    const int nodes = pentago::end::block_shape(shape, Vector<uint8_t,4>(b4)).product();
    const uint64_t node_off = dot(shape_strides, block_size*Vector<int,4>(b4));
    // Gather the block's nodes from the section slab (strided, not contiguous):
    // within-block order is C-order over the block shape, matching the kernel.
    const auto bshape = pentago::end::block_shape(shape, Vector<uint8_t,4>(b4));
    const auto bs = strides(bshape);
    vector<Vector<super_t,2>> gathered(nodes);
    for (int x0=0;x0<bshape[0];x0++) for (int x1=0;x1<bshape[1];x1++)
    for (int x2=0;x2<bshape[2];x2++) for (int x3=0;x3<bshape[3];x3++)
      gathered[dot(bs, vec(x0,x1,x2,x3))] = data[node_off + dot(shape_strides, vec(x0,x1,x2,x3))];
    compressed[b] = compress_block(asarray(gathered), level);
    auto& e = index[b];
    e.rel_off = data_len;
    e.comp_size = (uint32_t)compressed[b].size();
    e.uncomp_size = (uint32_t)(64*nodes);
    e.crc32 = (uint32_t)crc32(0, (const Bytef*)gathered.data(), 64*nodes);
    data_len += e.comp_size;
  }

  // Append payload
  uint64_t off = 64 + 48*header.section_count;
  for (int j=0;j<i;j++) off = std::max(off, entries[j].payload_off + entries[j].payload_len);
  xwrite(f, index.data(), 24*nblocks, off);
  uint64_t pos = off + 24*nblocks;
  for (uint64_t b=0;b<nblocks;b++) {
    xwrite(f, compressed[b].data(), compressed[b].size(), pos);
    pos += compressed[b].size();
  }

  // Update index entry and header
  entries[i].payload_off = off;
  entries[i].payload_len = 24*nblocks + data_len;
  entries[i].flags = SECTION_COMPLETE;
  xwrite(f, &entries[i], 48, 64 + 48*(uint64_t)i);
  header.sections_written++;
  xwrite(f, &header, sizeof(header), 0);
  fflush(f);
}

slice_reader_t::slice_reader_t(const string& path, uint64_t cache_bytes)
  : path(path), cache_bytes(cache_bytes) {
  f = fopen(path.c_str(), "rb");
  if (!f) die("open failed: " + path);
  xread(f, &header, sizeof(header), 0);
  if (memcmp(header.magic, slice_magic, 8) || header.version != slice_version)
    die("bad magic/version: " + path);
  entries.resize(header.section_count);
  if (header.section_count)
    xread(f, entries.data(), 48*header.section_count, 64);
  for (uint64_t i=0;i<header.section_count;i++)
    if (entries[i].flags & SECTION_COMPLETE)
      section_id[entries[i].sig] = (int)i;
}

slice_reader_t::~slice_reader_t() {
  if (f) fclose(f);
}

int slice_reader_t::find_section(const section_t& s) const {
  const auto it = section_id.find(s.sig());
  return it == section_id.end() ? -1 : it->second;
}

void slice_reader_t::read_block(int i, const Vector<int,4>& block,
                                RawArray<Vector<super_t,2>> dst) const {
  const auto& se = entries[i];
  const Vector<int,4> shape(se.shape[0], se.shape[1], se.shape[2], se.shape[3]);
  const Vector<int,4> nblocks((shape[0]+block_size-1)/block_size, (shape[1]+block_size-1)/block_size,
                              (shape[2]+block_size-1)/block_size, (shape[3]+block_size-1)/block_size);
  const uint64_t b = block_id(nblocks, block);
  const uint64_t key = (uint64_t)i<<32 | b;

  {
    std::lock_guard<std::mutex> lock(mutex);
    const auto it = cache.find(key);
    if (it != cache.end()) {
      it->second.lru = ++lru_clock;
      memcpy(dst.data(), it->second.data.data(), 64*dst.size());
      return;
    }
  }

  // Read index entry and data
  block_entry_t be;
  {
    std::lock_guard<std::mutex> lock(mutex);
    xread(f, &be, 24, se.payload_off + 24*b);
    vector<uint8_t> buf(be.comp_size);
    const uint64_t total_blocks = (uint64_t)nblocks.product();
    xread(f, buf.data(), be.comp_size, se.payload_off + 24*total_blocks + be.rel_off);
    if ((uint64_t)be.uncomp_size != 64*dst.size()) die("block size mismatch");
    uncompress_block(buf.data(), buf.size(), dst);
    if ((uint32_t)crc32(0, (const Bytef*)dst.data(), 64*dst.size()) != be.crc32)
      die("block crc mismatch");
  }

  // Insert into cache, evicting if needed
  std::lock_guard<std::mutex> lock(mutex);
  uint64_t used = 0;
  for (const auto& c : cache) used += 64*c.second.data.size();
  while (used + 64*dst.size() > cache_bytes && cache.size()) {
    auto worst = cache.begin();
    for (auto it = cache.begin(); it != cache.end(); ++it)
      if (it->second.lru < worst->second.lru) worst = it;
    used -= 64*worst->second.data.size();
    cache.erase(worst);
  }
  auto& c = cache[key];
  c.data.assign(dst.data(), dst.data()+dst.size());
  c.lru = ++lru_clock;
}

Vector<super_t,2> slice_reader_t::read_node(int i, uint64_t node_index) const {
  const Vector<int,4> shape(entries[i].shape[0], entries[i].shape[1],
                            entries[i].shape[2], entries[i].shape[3]);
  const Vector<int,4> nblocks((shape[0]+7)/8, (shape[1]+7)/8, (shape[2]+7)/8, (shape[3]+7)/8);
  // Find block and offset within block
  uint64_t rem = node_index;
  int x[4];
  for (int d=3;d>=0;d--) { x[d] = rem % shape[d]; rem /= shape[d]; }
  Vector<int,4> block(x[0]/8, x[1]/8, x[2]/8, x[3]/8);
  Vector<int,4> in_block(x[0]%8, x[1]%8, x[2]%8, x[3]%8);
  const auto bshape = pentago::end::block_shape(shape, Vector<uint8_t,4>(block));
  const uint64_t off = ((in_block[0]*bshape[1] + in_block[1])*bshape[2] + in_block[2])*bshape[3] + in_block[3];
  const uint64_t b = block_id(nblocks, block);
  const uint64_t key = (uint64_t)i<<32 | b;

  // Fast path: single node out of a cached block
  {
    std::lock_guard<std::mutex> lock(mutex);
    const auto it = cache.find(key);
    if (it != cache.end()) {
      it->second.lru = ++lru_clock;
      return it->second.data[off];
    }
  }
  vector<Vector<super_t,2>> buf(bshape.product());
  read_block(i, block, asarray(buf));
  return buf[off];
}

}  // namespace pgs
