#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>

#include "lsm/fs.hpp"
#include "lsm/rng.hpp"

namespace lsm {

// Thrown out of a SimFs call when an armed crash point is reached. The engine
// must not catch it; the test harness does.
struct SimCrash {
  std::uint64_t op;
};

// In-memory file system with a crash model.
//
// Each file tracks how many of its bytes are durable (covered by a sync()).
// crash() simulates power loss: every file keeps its durable bytes plus a
// random prefix of its unsynced bytes, so unsynced writes can vanish and the
// last write before a crash can be torn.
//
// Not modelled yet (see README "Not handled yet"):
//   - directory operations (create, rename, remove) are durable the moment
//     they happen, as if every one were followed by fsync of the directory
//   - unsynced bytes are only ever lost from the end; they are never
//     reordered, zeroed or replaced with garbage
//   - no bit flips, misdirected writes or I/O errors
class SimFs : public FileSystem {
 public:
  explicit SimFs(std::uint64_t seed) : rng_(seed) {}

  std::unique_ptr<WritableFile> create(const std::string& path) override;
  std::unique_ptr<ReadableFile> open(const std::string& path) override;
  bool exists(const std::string& path) override;
  void rename(const std::string& from, const std::string& to) override;
  void remove(const std::string& path) override;
  std::vector<std::string> list(const std::string& dir) override;
  void mkdir(const std::string& dir) override;

  // Throw SimCrash from the n-th mutating call from now (append, sync,
  // create, rename, remove; 1 = the next one). An append that hits the crash
  // point writes a random prefix of its data first.
  void arm_crash(std::uint64_t n) { crash_at_ = mutations_ + n; }
  void disarm() { crash_at_ = 0; }
  bool armed() const { return crash_at_ != 0; }
  // Power loss: drop a random suffix of every file's unsynced bytes.
  void crash();

  std::uint64_t mutations() const { return mutations_; }
  std::size_t file_count() const { return files_.size(); }
  // For tests.
  std::string contents(const std::string& path) const;
  std::uint64_t durable_size(const std::string& path) const;

  struct File {
    std::string data;
    std::size_t synced = 0;
  };

 private:
  friend class SimWritable;
  // Counts a mutation; throws SimCrash if it is the armed one.
  void mutate();

  Rng rng_;
  std::map<std::string, std::shared_ptr<File>> files_;
  std::uint64_t mutations_ = 0;
  std::uint64_t crash_at_ = 0;
};

}  // namespace lsm
