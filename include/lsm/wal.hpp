#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "lsm/fs.hpp"

namespace lsm {

// Write-ahead log. Each record is
//   fixed32 crc32c(len bytes + payload) | fixed32 len | payload
// Records are never split across anything; a log is just records back to
// back.
class LogWriter {
 public:
  explicit LogWriter(std::unique_ptr<WritableFile> file) : file_(std::move(file)) {}
  void add(std::string_view payload);
  void sync() { file_->sync(); }
  void close() { file_->close(); }

 private:
  std::unique_ptr<WritableFile> file_;
};

struct LogReadResult {
  std::vector<std::string> records;
  // True if the log ended in an incomplete or corrupt record, which is
  // expected after a crash during an append.
  bool torn_tail = false;
  std::uint64_t valid_bytes = 0;
};

// Reads records until the end of the log or the first record that is
// incomplete or fails its checksum. Everything after that point is ignored.
LogReadResult read_log(const ReadableFile& file);

}  // namespace lsm
