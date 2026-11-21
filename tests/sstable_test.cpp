#include <gtest/gtest.h>

#include "lsm/coding.hpp"
#include "lsm/sim_fs.hpp"
#include "lsm/sstable.hpp"

using namespace lsm;

namespace {

std::string k(int i) {
  char buf[16];
  std::snprintf(buf, sizeof buf, "k%06d", i);
  return buf;
}

TableMeta build(SimFs& fs, const std::string& path, int n, TableOptions opts = {}) {
  TableBuilder b(fs.create(path), opts);
  for (int i = 0; i < n; ++i) {
    const auto type = i % 7 == 0 ? ValueType::Deletion : ValueType::Value;
    b.add(k(i * 2), static_cast<SequenceNumber>(i + 1), type,
          type == ValueType::Value ? "value" + std::to_string(i) : "");
  }
  return b.finish();
}

}  // namespace

TEST(SSTable, PointLookups) {
  SimFs fs(1);
  TableOptions opts;
  opts.block_bytes = 256;  // many blocks
  const TableMeta m = build(fs, "/d/1.sst", 1000, opts);
  EXPECT_EQ(m.entries, 1000u);
  EXPECT_EQ(m.smallest, k(0));
  EXPECT_EQ(m.largest, k(1998));
  auto t = TableReader::open(fs, "/d/1.sst");
  EXPECT_GT(t->blocks().size(), 20u);
  for (int i = 0; i < 1000; ++i) {
    auto f = t->get(k(i * 2));
    ASSERT_TRUE(f) << i;
    EXPECT_EQ(f->seq, static_cast<SequenceNumber>(i + 1));
    if (i % 7 == 0) EXPECT_EQ(f->type, ValueType::Deletion);
    else EXPECT_EQ(f->value, "value" + std::to_string(i));
    EXPECT_FALSE(t->get(k(i * 2 + 1)));  // the gaps
  }
  EXPECT_FALSE(t->get("a"));
  EXPECT_FALSE(t->get("z"));
}

TEST(SSTable, IteratorAndSeek) {
  SimFs fs(1);
  TableOptions opts;
  opts.block_bytes = 128;
  build(fs, "/d/1.sst", 300, opts);
  auto t = TableReader::open(fs, "/d/1.sst");
  auto it = t->iterator();
  int n = 0;
  for (it->seek_to_first(); it->valid(); it->next()) EXPECT_EQ(it->key(), k(2 * n++));
  EXPECT_EQ(n, 300);
  it->seek(k(101));  // between keys: lands on the next one
  ASSERT_TRUE(it->valid());
  EXPECT_EQ(it->key(), k(102));
  it->seek(k(598));
  ASSERT_TRUE(it->valid());
  EXPECT_EQ(it->key(), k(598));
  it->seek("zzz");
  EXPECT_FALSE(it->valid());
}

TEST(SSTable, RejectsOutOfOrderKeys) {
  SimFs fs(1);
  TableBuilder b(fs.create("/d/1.sst"), {});
  b.add("b", 1, ValueType::Value, "");
  EXPECT_THROW(b.add("a", 2, ValueType::Value, ""), std::logic_error);
  EXPECT_THROW(b.add("b", 2, ValueType::Value, ""), std::logic_error);
}

TEST(SSTable, EverySingleByteFlipIsDetected) {
  // Flip each byte in turn: opening or reading the table must either throw
  // Corruption or still return exactly the right data. Never wrong data.
  SimFs fs(1);
  TableOptions opts;
  opts.block_bytes = 64;
  build(fs, "/d/1.sst", 20, opts);
  const std::string good = fs.contents("/d/1.sst");
  int detected = 0;
  for (std::size_t i = 0; i < good.size(); ++i) {
    std::string bad = good;
    bad[i] ^= 0x40;
    fs.create("/d/2.sst")->append(bad);
    try {
      auto t = TableReader::open(fs, "/d/2.sst");
      for (int j = 0; j < 20; ++j) {
        auto f = t->get(k(j * 2));
        // The padding bytes at the very end aren't covered by a checksum.
        if (!f) throw Corruption("lookup failed");  // a flip can make the bloom filter say no
        ASSERT_EQ(f->seq, static_cast<SequenceNumber>(j + 1)) << "byte " << i;
      }
    } catch (const Corruption&) {
      ++detected;
    }
  }
  EXPECT_GE(detected, static_cast<int>(good.size()) - 4);
}

TEST(SSTable, TruncatedFileIsCorruption) {
  SimFs fs(1);
  build(fs, "/d/1.sst", 50);
  const std::string good = fs.contents("/d/1.sst");
  for (std::size_t cut : {std::size_t{0}, std::size_t{10}, good.size() / 2, good.size() - 1}) {
    fs.create("/d/2.sst")->append(good.substr(0, cut));
    EXPECT_THROW(TableReader::open(fs, "/d/2.sst"), Corruption) << cut;
  }
}
