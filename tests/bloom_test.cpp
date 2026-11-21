#include <gtest/gtest.h>

#include "lsm/bloom.hpp"

using namespace lsm;

TEST(Bloom, NoFalseNegatives) {
  std::vector<std::string> keys;
  for (int i = 0; i < 5000; ++i) keys.push_back("key" + std::to_string(i));
  const std::string f = BloomFilter::build(keys, 10);
  for (const auto& k : keys) ASSERT_TRUE(BloomFilter::may_contain(f, k)) << k;
}

TEST(Bloom, FalsePositiveRateNearTheory) {
  // 10 bits/key and 6 probes: about 0.8% in theory.
  std::vector<std::string> keys;
  for (int i = 0; i < 10000; ++i) keys.push_back("in" + std::to_string(i));
  const std::string f = BloomFilter::build(keys, 10);
  int fp = 0;
  for (int i = 0; i < 100000; ++i) fp += BloomFilter::may_contain(f, "out" + std::to_string(i));
  EXPECT_LT(fp, 2000);  // < 2%
  EXPECT_GT(fp, 100);   // and not suspiciously perfect
}

TEST(Bloom, EmptyFilterAndTinyKeySets) {
  const std::string f = BloomFilter::build({}, 10);
  int hits = 0;
  for (int i = 0; i < 100; ++i) hits += BloomFilter::may_contain(f, std::to_string(i));
  EXPECT_EQ(hits, 0);
  const std::string g = BloomFilter::build({"only"}, 10);
  EXPECT_TRUE(BloomFilter::may_contain(g, "only"));
}
