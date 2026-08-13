// On-disk format for one slice of the Pentago WDL database ("*.pgs" files)
//
// A slice file stores the values of all positions with a fixed number of stones,
// organized into sections (see girving/pentago/base/section.h).  Only sections
// that are lexicographically minimal under D4 symmetry are stored.
//
// Each node is a Vector<super_t,2> = (wins, not_loss), 64 bytes, encoding the
// win/draw/loss value of the 256 quadrant rotations of one board (see
// girving/pentago/base/superscore.h).  Nodes are stored in 8x8x8x8 blocks
// (girving/pentago/end/config.h: block_size = 8), block-major, C-order both
// within and between blocks, matching the in-memory conventions of
// girving/pentago/end/compute.cc.
//
// File layout (all integers little endian):
//
//   [0, 64)                 header (struct slice_header_t)
//   [64, 64+48*sections)    section index (struct section_entry_t)
//   64+48*sections ...      section payloads, appended in write order
//
// Section payload:
//   [0, 24*blocks)          block index (struct block_entry_t)
//   24*blocks ...           zlib-compressed block data, concatenated
//
// Blocks may be partial at section boundaries; uncompressed size is always
// block_nodes * 64 where block_nodes = product(block_shape(shape, block)).
//
// Restart rule: section payloads are appended in section order; a section is
// valid only if its entry has flag SECTION_COMPLETE set.  On restart, the file
// is truncated past the last complete section.
#pragma once

#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <zlib.h>
#include "pentago/base/section.h"
#include "pentago/base/superscore.h"
#include "pentago/end/blocks.h"
#include "pentago/utility/vector.h"

namespace pgs {

using namespace pentago;
using pentago::end::block_size;

const char slice_magic[8] = {'P','G','S','L','I','C','E','1'};
const uint32_t slice_version = 1;
const uint32_t SECTION_COMPLETE = 1;

struct slice_header_t {
  char magic[8];
  uint32_t version;
  uint32_t slice;
  uint64_t section_count;    // total sections in this slice
  uint64_t sections_written; // sections with SECTION_COMPLETE (restart cursor)
  uint8_t reserved[32];
};
static_assert(sizeof(slice_header_t)==64, "header size");

struct section_entry_t {
  uint64_t sig;              // section_t::sig()
  int32_t shape[4];
  uint64_t payload_off;      // absolute file offset of payload
  uint64_t payload_len;
  uint32_t flags;
  uint32_t reserved;
};
static_assert(sizeof(section_entry_t)==48, "section entry size");

struct block_entry_t {
  uint64_t rel_off;          // relative to start of block data region
  uint32_t comp_size;
  uint32_t uncomp_size;      // nodes*64
  uint32_t crc32;            // crc32 of uncompressed data
  uint32_t reserved;
};
static_assert(sizeof(block_entry_t)==24, "block entry size");

// Number of blocks along each dimension, and total
static inline Vector<int,4> section_blocks(const section_t& s) {
  return pentago::end::section_blocks(s);
}

// Flat block id in C-order
static inline uint64_t block_id(const Vector<int,4>& blocks, const Vector<int,4>& block) {
  uint64_t id = 0;
  for (int i=0;i<4;i++) id = id*blocks[i] + block[i];
  return id;
}

// Uncompressed byte count of a block
static inline uint64_t block_bytes(const Vector<int,4>& shape, const Vector<uint8_t,4>& block) {
  return 64*uint64_t(pentago::end::block_shape(shape, block).product());
}

// Compress/uncompress one block
std::vector<uint8_t> compress_block(RawArray<const Vector<super_t,2>> data, int level);
void uncompress_block(const uint8_t* src, size_t src_size,
                      RawArray<Vector<super_t,2>> dst); // dst.size() == uncomp/64

// Append-only writer for one slice file
struct slice_writer_t {
  const std::string path;
  const int level; // zlib level
  FILE* f = 0;
  slice_header_t header = {};
  std::vector<section_entry_t> entries;

  // Open for writing; if the file exists, complete sections are kept and the
  // file is truncated past the last complete section (restart).
  slice_writer_t(const std::string& path, int slice,
                 RawArray<const section_t> sections, int level=6);
  ~slice_writer_t();

  int sections_done() const { return (int)header.sections_written; }
  bool section_done(int i) const { return entries[i].flags & SECTION_COMPLETE; }

  // Write one complete section (its blocks must be in C-order, uncompressed).
  // data.size() must equal sections[i].size().
  void write_section(int i, const section_t& section,
                     RawArray<const Vector<super_t,2>> data);
};

// Random-access reader for one slice file, with an LRU cache of uncompressed blocks
struct slice_reader_t {
  const std::string path;
  FILE* f = 0;
  slice_header_t header = {};
  std::vector<section_entry_t> entries;
  std::unordered_map<uint64_t,int> section_id; // sig -> index

  // Block cache
  const uint64_t cache_bytes;
  mutable std::mutex mutex;
  struct cached_t { std::vector<Vector<super_t,2>> data; uint64_t lru; };
  mutable std::unordered_map<uint64_t,cached_t> cache; // (section<<32)|block -> data
  mutable uint64_t lru_clock = 0;

  slice_reader_t(const std::string& path, uint64_t cache_bytes=(uint64_t)4<<30);
  ~slice_reader_t();

  int slice() const { return header.slice; }
  int find_section(const section_t& s) const; // -1 if absent
  const section_entry_t& entry(int i) const { return entries[i]; }

  // Copy block `block` of section i into dst (dst.size() == block nodes).
  void read_block(int i, const Vector<int,4>& block,
                  RawArray<Vector<super_t,2>> dst) const;

  // Read a single node (slow; for lookup tools).
  Vector<super_t,2> read_node(int i, uint64_t node_index) const;
};

}  // namespace pgs
