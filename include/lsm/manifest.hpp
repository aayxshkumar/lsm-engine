#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "lsm/format.hpp"
#include "lsm/fs.hpp"

namespace lsm {

struct FileMeta {
  int level = 0;
  std::uint64_t number = 0;
  std::uint64_t size = 0;
  std::string smallest;
  std::string largest;
};

// The whole durable state of the tree apart from the tables and logs
// themselves. Rewritten in full on every change: written to MANIFEST.tmp,
// synced, then renamed over MANIFEST.
struct Manifest {
  std::uint64_t next_file = 1;
  SequenceNumber last_sequence = 0;
  // Logs numbered below this are fully contained in tables.
  std::uint64_t log_number = 0;
  std::vector<FileMeta> files;

  std::string encode() const;
  static Manifest decode(std::string_view data);  // throws Corruption
};

void write_manifest(FileSystem& fs, const std::string& dir, const Manifest& m);
// Returns false if there is no MANIFEST (a new database).
bool read_manifest(FileSystem& fs, const std::string& dir, Manifest& out);

}  // namespace lsm
