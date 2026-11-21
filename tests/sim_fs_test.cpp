#include <gtest/gtest.h>

#include "lsm/sim_fs.hpp"

using namespace lsm;

TEST(SimFs, SyncedDataSurvivesCrash) {
  SimFs fs(1);
  auto f = fs.create("/d/a");
  f->append("durable");
  f->sync();
  f->append("-maybe");
  for (int i = 0; i < 20; ++i) {
    fs.crash();
    const std::string c = fs.contents("/d/a");
    ASSERT_GE(c.size(), 7u);
    EXPECT_EQ(c.substr(0, 7), "durable");
    EXPECT_EQ(std::string("durable-maybe").substr(0, c.size()), c);  // always a prefix
  }
}

TEST(SimFs, UnsyncedDataCanBeLostOrTorn) {
  bool lost = false, kept = false, torn = false;
  for (std::uint64_t seed = 1; seed <= 200; ++seed) {
    SimFs fs(seed);
    auto f = fs.create("/d/a");
    f->append("0123456789");
    fs.crash();
    const auto n = fs.contents("/d/a").size();
    lost |= n == 0;
    kept |= n == 10;
    torn |= n > 0 && n < 10;
  }
  EXPECT_TRUE(lost);
  EXPECT_TRUE(kept);
  EXPECT_TRUE(torn);
}

TEST(SimFs, ArmedCrashThrowsOnTheNthMutation) {
  SimFs fs(2);
  auto f = fs.create("/d/a");  // mutation 1
  fs.arm_crash(3);
  f->append("x");  // 2
  f->append("y");  // 3
  EXPECT_THROW(f->sync(), SimCrash);  // 4 = armed
  EXPECT_FALSE(fs.armed());
  EXPECT_NO_THROW(f->sync());
}

TEST(SimFs, CrashingAppendWritesAPrefix) {
  for (std::uint64_t seed = 1; seed <= 50; ++seed) {
    SimFs fs(seed);
    auto f = fs.create("/d/a");
    fs.arm_crash(1);
    EXPECT_THROW(f->append("abcdefgh"), SimCrash);
    const std::string c = fs.contents("/d/a");
    EXPECT_LT(c.size(), 8u);
    EXPECT_EQ(std::string("abcdefgh").substr(0, c.size()), c);
  }
}

TEST(SimFs, RenameReplacesAndListShowsNames) {
  SimFs fs(3);
  fs.create("/d/x")->append("old");
  auto t = fs.create("/d/x.tmp");
  t->append("new");
  t->sync();
  fs.rename("/d/x.tmp", "/d/x");
  EXPECT_EQ(fs.contents("/d/x"), "new");
  EXPECT_FALSE(fs.exists("/d/x.tmp"));
  fs.create("/d/sub/y");
  fs.create("/e/z");
  EXPECT_EQ(fs.list("/d"), (std::vector<std::string>{"x"}));
}
