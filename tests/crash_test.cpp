#include <gtest/gtest.h>

#include <set>

#include "lsm/crash_harness.hpp"
#include "lsm/sim_fs.hpp"

using namespace lsm;

TEST(Crash, SyncedWriteSurvivesUnsyncedMayNot) {
  SimFs fs(1);
  {
    auto db = DB::open(fs, "/db");
    db->put("a", "synced", WriteOptions{true});
    db->put("b", "unsynced");
  }
  fs.crash();
  auto db = DB::open(fs, "/db");
  EXPECT_EQ(db->get("a"), "synced");
  // "b" may or may not be there, but if it is, it's intact.
  if (auto b = db->get("b")) EXPECT_EQ(*b, "unsynced");
}

TEST(Crash, LossOfUnsyncedWritesHappensAndIsAPrefix) {
  // Ten unsynced writes then power loss: across seeds we should see every
  // amount survive, and always a prefix.
  std::set<std::size_t> survived;
  for (std::uint64_t seed = 1; seed <= 100; ++seed) {
    SimFs fs(seed);
    {
      auto db = DB::open(fs, "/db");
      for (int i = 0; i < 10; ++i) db->put("k" + std::to_string(i), "v");
    }
    fs.crash();
    auto db = DB::open(fs, "/db");
    std::size_t n = 0;
    while (n < 10 && db->get("k" + std::to_string(n))) ++n;
    for (std::size_t i = n; i < 10; ++i) EXPECT_FALSE(db->get("k" + std::to_string(i))) << seed;
    EXPECT_EQ(db->last_sequence(), n);
    survived.insert(n);
  }
  EXPECT_TRUE(survived.count(0));
  EXPECT_TRUE(survived.count(10));
  EXPECT_GT(survived.size(), 5u);
}

TEST(Crash, SameSeedSameRun) {
  CrashConfig cfg;
  cfg.seed = 9;
  const auto a = run_crash_test(cfg);
  const auto b = run_crash_test(cfg);
  EXPECT_EQ(a.fs_ops, b.fs_ops);
  EXPECT_EQ(a.writes_acked, b.writes_acked);
  EXPECT_EQ(a.crashes, b.crashes);
}

TEST(Crash, HarnessExercisesTheHardPaths) {
  // Over a few seeds, crashes must land during recovery, recoveries must
  // see torn logs, and the tree must flush and compact; otherwise the sweep
  // below proves less than it claims.
  CrashResult total;
  for (std::uint64_t seed = 1; seed <= 10; ++seed) {
    CrashConfig cfg;
    cfg.seed = seed;
    const auto r = run_crash_test(cfg);
    ASSERT_TRUE(r.ok()) << "seed " << seed << ": " << r.failure;
    total.crashes += r.crashes;
    total.crashes_in_open += r.crashes_in_open;
    total.torn_logs += r.torn_logs;
    total.flushes += r.flushes;
    total.compactions += r.compactions;
  }
  EXPECT_GT(total.crashes, 50);
  EXPECT_GT(total.crashes_in_open, 10);
  EXPECT_GT(total.torn_logs, 10);
  EXPECT_GT(total.flushes, 100u);
  EXPECT_GT(total.compactions, 20u);
}

TEST(Crash, SeedSweep) {
  for (std::uint64_t seed = 1; seed <= 200; ++seed) {
    CrashConfig cfg;
    cfg.seed = seed;
    const auto r = run_crash_test(cfg);
    ASSERT_TRUE(r.ok()) << "seed " << seed << ": " << r.failure;
  }
}
