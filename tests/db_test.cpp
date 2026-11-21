#include <gtest/gtest.h>

#include <cstdio>

#include <map>

#include "lsm/db.hpp"
#include "lsm/rng.hpp"
#include "lsm/sim_fs.hpp"

using namespace lsm;

namespace {

Options tiny() {
  Options o;
  o.memtable_bytes = 2048;
  o.target_file_bytes = 4096;
  o.l0_compaction_trigger = 3;
  o.table.block_bytes = 256;
  return o;
}

std::string key(std::uint64_t i) {
  char buf[16];
  std::snprintf(buf, sizeof buf, "key%04llu", static_cast<unsigned long long>(i));
  return buf;
}

}  // namespace

TEST(DB, PutGetDelete) {
  SimFs fs(1);
  auto db = DB::open(fs, "/db");
  EXPECT_FALSE(db->get("a"));
  db->put("a", "1");
  db->put("b", "2");
  db->put("a", "3");
  EXPECT_EQ(db->get("a"), "3");
  db->del("b");
  EXPECT_FALSE(db->get("b"));
  db->flush();
  EXPECT_EQ(db->get("a"), "3");
  EXPECT_FALSE(db->get("b"));
}

TEST(DB, DeletionHidesOlderValueInLowerLevel) {
  SimFs fs(1);
  auto db = DB::open(fs, "/db", tiny());
  db->put("k", "old");
  db->flush();
  db->compact();  // "old" now in L1
  ASSERT_EQ(db->stats().l1_files, 1u);
  db->del("k");
  db->flush();  // tombstone in L0
  EXPECT_FALSE(db->get("k"));
  EXPECT_TRUE(db->scan("").empty());
  db->compact();  // tombstone and value both dropped
  EXPECT_FALSE(db->get("k"));
  EXPECT_EQ(db->stats().l1_files, 0u);
}

TEST(DB, ReopenWithoutCrashKeepsEverything) {
  SimFs fs(1);
  {
    auto db = DB::open(fs, "/db", tiny());
    for (int i = 0; i < 500; ++i) db->put(key(static_cast<std::uint64_t>(i)), "v" + std::to_string(i));
  }
  auto db = DB::open(fs, "/db", tiny());
  for (int i = 0; i < 500; ++i) EXPECT_EQ(db->get(key(static_cast<std::uint64_t>(i))), "v" + std::to_string(i));
  EXPECT_EQ(db->last_sequence(), 500u);
}

TEST(DB, ScanRangesAndLimit) {
  SimFs fs(1);
  auto db = DB::open(fs, "/db", tiny());
  for (std::uint64_t i = 0; i < 100; ++i) db->put(key(i), std::to_string(i));
  for (std::uint64_t i = 0; i < 100; i += 3) db->del(key(i));
  const auto rows = db->scan(key(10), key(20));
  std::vector<std::string> keys;
  for (const auto& [k, v] : rows) keys.push_back(k);
  EXPECT_EQ(keys, (std::vector<std::string>{key(10), key(11), key(13), key(14), key(16), key(17), key(19)}));
  EXPECT_EQ(db->scan("", "", 5).size(), 5u);
  EXPECT_EQ(db->scan(key(99)).size(), 0u);  // 99 was deleted
}

TEST(DB, CompactionIsTriggeredAndBounded) {
  SimFs fs(1);
  auto db = DB::open(fs, "/db", tiny());
  for (std::uint64_t i = 0; i < 5000; ++i) db->put(key(i % 300), std::string(50, 'a' + i % 26));
  const DbStats s = db->stats();
  EXPECT_GT(s.flushes, 20u);
  EXPECT_GT(s.compactions, 5u);
  EXPECT_LT(s.l0_files, 3u);
  // L1 files are sorted and disjoint.
  std::string prev_largest;
  for (const auto& f : db->manifest().files) {
    if (f.level != 1) continue;
    EXPECT_GT(f.smallest, prev_largest);
    EXPECT_LE(f.smallest, f.largest);
    prev_largest = f.largest;
  }
}

TEST(DB, ObsoleteFilesAreDeleted) {
  SimFs fs(1);
  auto db = DB::open(fs, "/db", tiny());
  for (std::uint64_t i = 0; i < 3000; ++i) db->put(key(i % 100), "x");
  // MANIFEST + one log + live tables, nothing else.
  const auto names = fs.list("/db");
  const DbStats s = db->stats();
  EXPECT_EQ(names.size(), 2 + s.l0_files + s.l1_files);
}

TEST(DB, BloomFilterSkipsMissingKeys) {
  SimFs fs(1);
  auto db = DB::open(fs, "/db", tiny());
  for (std::uint64_t i = 0; i < 1000; ++i) db->put(key(i * 2), "x");
  db->flush();
  for (std::uint64_t i = 0; i < 1000; ++i) EXPECT_FALSE(db->get(key(i * 2 + 1)));
  EXPECT_GT(db->stats().bloom_skips, 900u);
}

TEST(DB, RandomOpsMatchModelAcrossReopens) {
  // Differential test: random puts, deletes, batches, flushes, compactions
  // and clean reopens, checked against std::map after every step.
  for (std::uint64_t seed = 1; seed <= 20; ++seed) {
    SimFs fs(seed);
    Rng rng(seed);
    std::map<std::string, std::string> model;
    auto db = DB::open(fs, "/db", tiny());
    for (int step = 0; step < 3000; ++step) {
      const auto r = rng.uniform(0, 999);
      const std::string k = key(rng.uniform(0, 199));
      if (r < 500) {
        const std::string v = std::to_string(step) + std::string(rng.uniform(0, 60), 'v');
        db->put(k, v);
        model[k] = v;
      } else if (r < 700) {
        db->del(k);
        model.erase(k);
      } else if (r < 750) {
        WriteBatch b;
        for (int j = 0; j < 4; ++j) {
          const std::string bk = key(rng.uniform(0, 199));
          if (j % 2) {
            b.put(bk, "batch" + std::to_string(step));
            model[bk] = "batch" + std::to_string(step);
          } else {
            b.del(bk);
            model.erase(bk);
          }
        }
        db->write(b);
      } else if (r < 760) {
        db->flush();
      } else if (r < 765) {
        db->compact();
      } else if (r < 770) {
        db.reset();
        db = DB::open(fs, "/db", tiny());
      } else {
        auto it = model.find(k);
        const auto want = it == model.end() ? std::nullopt : std::optional<std::string>(it->second);
        ASSERT_EQ(db->get(k), want) << "seed " << seed << " step " << step;
      }
    }
    const auto rows = db->scan("");
    const std::map<std::string, std::string> actual(rows.begin(), rows.end());
    ASSERT_EQ(actual, model) << "seed " << seed;
  }
}

TEST(DB, WorksOnPosixFs) {
  PosixFs fs;
  const std::string dir = ::testing::TempDir() + "lsm_posix_test";
  fs.mkdir(dir);
  for (const auto& f : fs.list(dir)) fs.remove(dir + "/" + f);
  {
    auto db = DB::open(fs, dir, tiny());
    for (std::uint64_t i = 0; i < 2000; ++i) db->put(key(i), std::to_string(i), WriteOptions{i % 500 == 0});
  }
  auto db = DB::open(fs, dir, tiny());
  for (std::uint64_t i = 0; i < 2000; i += 37) EXPECT_EQ(db->get(key(i)), std::to_string(i));
  db.reset();
  for (const auto& f : fs.list(dir)) fs.remove(dir + "/" + f);
}
