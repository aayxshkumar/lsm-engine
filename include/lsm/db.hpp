#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "lsm/format.hpp"
#include "lsm/fs.hpp"
#include "lsm/manifest.hpp"
#include "lsm/memtable.hpp"
#include "lsm/sstable.hpp"
#include "lsm/wal.hpp"

namespace lsm {

struct Options {
  std::size_t memtable_bytes = 4 << 20;
  // Compact when level 0 has this many tables.
  int l0_compaction_trigger = 4;
  std::size_t target_file_bytes = 2 << 20;
  TableOptions table;
};

struct WriteOptions {
  // Sync the WAL before returning, so the write survives a crash.
  bool sync = false;
};

struct DbStats {
  std::uint64_t flushes = 0;
  std::uint64_t compactions = 0;
  std::uint64_t user_bytes_written = 0;
  std::uint64_t bloom_skips = 0;
  std::size_t l0_files = 0;
  std::size_t l1_files = 0;
  std::size_t memtable_entries = 0;
  bool recovered_torn_log = false;
};

// A two-level LSM tree:
//
//   writes -> WAL + memtable --flush--> L0 tables (may overlap, newest first)
//                                    --compaction--> L1 tables (sorted, disjoint)
//
// Single-threaded: flushes and compactions run inline in the write that
// triggers them. Not thread-safe.
//
// Durability: a write with sync=true is durable when it returns, along with
// every write before it. A write without sync may be lost in a crash, but
// recovery always yields a prefix of the writes in the order they were made
// (never a later write without an earlier one), with batches all-or-nothing.
class DB {
 public:
  static std::unique_ptr<DB> open(FileSystem& fs, std::string dir, Options opts = {});

  DB(const DB&) = delete;
  DB& operator=(const DB&) = delete;
  ~DB();

  void put(std::string_view key, std::string_view value, WriteOptions wo = {});
  void del(std::string_view key, WriteOptions wo = {});
  void write(const WriteBatch& batch, WriteOptions wo = {});

  std::optional<std::string> get(std::string_view key);
  // Live entries with start <= key < end (end empty = no upper bound), at
  // most `limit` of them.
  std::vector<std::pair<std::string, std::string>> scan(std::string_view start,
                                                         std::string_view end = {},
                                                         std::size_t limit = SIZE_MAX);

  // Writes the memtable to a level-0 table, even if it isn't full.
  void flush();
  // Merges all of L0 and L1 into a new L1.
  void compact();

  SequenceNumber last_sequence() const { return last_seq_; }
  DbStats stats() const;
  const Manifest& manifest() const { return manifest_; }

 private:
  DB(FileSystem& fs, std::string dir, Options opts);
  void recover();
  void maybe_compact();
  void apply_manifest(Manifest next);
  std::shared_ptr<TableReader> table(std::uint64_t number);
  std::string path(std::uint64_t number, const char* ext) const;
  void delete_obsolete_files();

  FileSystem& fs_;
  std::string dir_;
  Options opts_;
  Manifest manifest_;
  SequenceNumber last_seq_ = 0;
  MemTable mem_;
  std::unique_ptr<LogWriter> log_;
  std::uint64_t log_file_ = 0;
  std::map<std::uint64_t, std::shared_ptr<TableReader>> tables_;
  DbStats stats_;
};

}  // namespace lsm
