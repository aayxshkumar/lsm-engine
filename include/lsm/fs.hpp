#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace lsm {

// Append-only file being written. Data is durable only after sync().
class WritableFile {
 public:
  virtual ~WritableFile() = default;
  virtual void append(std::string_view data) = 0;
  virtual void sync() = 0;
  virtual void close() = 0;
};

class ReadableFile {
 public:
  virtual ~ReadableFile() = default;
  virtual std::uint64_t size() const = 0;
  // Reads up to n bytes at offset; returns fewer only at end of file.
  virtual std::string read(std::uint64_t offset, std::size_t n) const = 0;
};

struct FsStats {
  std::uint64_t bytes_written = 0;
  std::uint64_t bytes_read = 0;
  std::uint64_t syncs = 0;
  std::uint64_t files_created = 0;
};

// The engine does all I/O through this interface, so tests can swap in SimFs
// (in memory, with crash injection) for the real POSIX one.
class FileSystem {
 public:
  virtual ~FileSystem() = default;
  // Creates or truncates.
  virtual std::unique_ptr<WritableFile> create(const std::string& path) = 0;
  virtual std::unique_ptr<ReadableFile> open(const std::string& path) = 0;
  virtual bool exists(const std::string& path) = 0;
  // Atomically replaces `to` if it exists.
  virtual void rename(const std::string& from, const std::string& to) = 0;
  virtual void remove(const std::string& path) = 0;
  // File names (not paths) directly inside dir.
  virtual std::vector<std::string> list(const std::string& dir) = 0;
  virtual void mkdir(const std::string& dir) = 0;

  const FsStats& stats() const { return stats_; }

 protected:
  FsStats stats_;
};

// Real files, via POSIX open/write/fsync/pread/rename.
class PosixFs : public FileSystem {
 public:
  std::unique_ptr<WritableFile> create(const std::string& path) override;
  std::unique_ptr<ReadableFile> open(const std::string& path) override;
  bool exists(const std::string& path) override;
  void rename(const std::string& from, const std::string& to) override;
  void remove(const std::string& path) override;
  std::vector<std::string> list(const std::string& dir) override;
  void mkdir(const std::string& dir) override;
};

}  // namespace lsm
