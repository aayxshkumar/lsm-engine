#pragma once

#include <cstdint>
#include <string>

#include "lsm/db.hpp"

namespace lsm {

struct CrashConfig {
  std::uint64_t seed = 1;
  int rounds = 15;              // crash/recover cycles
  int ops_per_round = 400;      // most writes attempted before a crash
  int keys = 64;
  std::size_t max_value = 120;
  double sync_prob = 0.15;      // write with WriteOptions{.sync = true}
  double delete_prob = 0.2;
  double batch_prob = 0.15;     // multi-op batch instead of a single op
  double read_prob = 0.15;      // get() checked against the model
  double crash_in_open_prob = 0.3;
  // Small sizes so a run flushes and compacts many times.
  Options opts = [] {
    Options o;
    o.memtable_bytes = 6 * 1024;
    o.target_file_bytes = 8 * 1024;
    o.l0_compaction_trigger = 3;
    o.table.block_bytes = 512;
    return o;
  }();
};

struct CrashResult {
  bool ok() const { return failure.empty(); }
  std::string failure;  // first failure, empty if the run passed
  int crashes = 0;
  int crashes_in_open = 0;
  std::uint64_t writes_acked = 0;
  std::uint64_t reads_checked = 0;
  std::uint64_t flushes = 0;
  std::uint64_t compactions = 0;
  int torn_logs = 0;  // recoveries that found a torn WAL tail
  std::uint64_t fs_ops = 0;
};

// One seeded crash-recovery run on SimFs.
//
// The harness keeps a model of the database and, after every recovery,
// checks that the recovered contents are exactly the model after some prefix
// of the attempted batches, where the prefix
//   - includes every batch acknowledged before a sync write, a flush, or a
//     successful open returned (those are durable), and
//   - ends on a batch boundary (batches are atomic).
// Between crashes, every get() is checked against the model too.
CrashResult run_crash_test(const CrashConfig& cfg);

}  // namespace lsm
