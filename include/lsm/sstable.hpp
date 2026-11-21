#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "lsm/format.hpp"
#include "lsm/fs.hpp"
#include "lsm/iterator.hpp"

namespace lsm {

// Sorted string table: an immutable file of entries with unique user keys.
//
//   data block*     entries, then fixed32 crc32c(entries)
//   filter block    bloom filter over all keys, then fixed32 crc32c
//   index block     per data block: bytes last_key, fixed64 offset,
//                   fixed32 size; then fixed32 crc32c
//   footer (48 B)   fixed64 index_off, fixed32 index_size,
//                   fixed64 filter_off, fixed32 filter_size,
//                   fixed64 entry_count, fixed64 magic,
//                   fixed32 crc32c(first 40 bytes), 4 bytes padding
//
// An entry is bytes key, fixed64 seq, u8 type, bytes value. Sizes in the
// footer and index exclude the 4-byte crc trailer.
//
// Each table holds at most one version per user key: flushes and
// compactions keep only the newest, since there are no snapshots yet.

struct TableOptions {
  std::size_t block_bytes = 4096;
  int bloom_bits_per_key = 10;
};

struct TableMeta {
  std::uint64_t file_size = 0;
  std::uint64_t entries = 0;
  std::string smallest;
  std::string largest;
};

class TableBuilder {
 public:
  TableBuilder(std::unique_ptr<WritableFile> file, TableOptions opts);
  // Keys must be strictly increasing.
  void add(std::string_view key, SequenceNumber seq, ValueType type, std::string_view value);
  std::uint64_t estimated_size() const { return offset_ + block_.size(); }
  std::uint64_t entries() const { return entries_; }
  // Writes filter, index and footer, syncs and closes the file.
  TableMeta finish();

 private:
  void flush_block();
  void write_with_crc(const std::string& payload);

  std::unique_ptr<WritableFile> file_;
  TableOptions opts_;
  std::string block_;
  std::string index_;
  std::vector<std::string> keys_;
  std::string last_key_;
  std::uint64_t offset_ = 0;
  std::uint64_t entries_ = 0;
  TableMeta meta_;
};

class TableReader {
 public:
  // Reads and checks the footer, index and filter. Throws Corruption.
  static std::shared_ptr<TableReader> open(FileSystem& fs, const std::string& path);

  struct Found {
    SequenceNumber seq;
    ValueType type;
    std::string value;
  };
  std::optional<Found> get(std::string_view key) const;
  std::unique_ptr<InternalIterator> iterator() const;

  std::uint64_t entries() const { return entries_; }
  // How many get() calls the bloom filter answered without a block read.
  mutable std::uint64_t bloom_skips = 0;

  struct BlockHandle {
    std::string last_key;
    std::uint64_t offset;
    std::uint32_t size;
  };
  // Reads a data block and checks its crc. Throws Corruption.
  std::string read_block(const BlockHandle& h) const;
  const std::vector<BlockHandle>& blocks() const { return index_; }

 private:
  std::unique_ptr<ReadableFile> file_;
  std::string path_;
  std::vector<BlockHandle> index_;
  std::string filter_;
  std::uint64_t entries_ = 0;
};

}  // namespace lsm
