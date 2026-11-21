#include <gtest/gtest.h>

#include "lsm/format.hpp"
#include "lsm/sim_fs.hpp"
#include "lsm/wal.hpp"

using namespace lsm;

namespace {

LogReadResult read_back(SimFs& fs, const std::string& path) { return read_log(*fs.open(path)); }

}  // namespace

TEST(Wal, RoundTrip) {
  SimFs fs(1);
  {
    LogWriter w(fs.create("/d/1.log"));
    w.add("one");
    w.add("");
    w.add(std::string(10000, 'z'));
  }
  const auto r = read_back(fs, "/d/1.log");
  ASSERT_EQ(r.records.size(), 3u);
  EXPECT_EQ(r.records[0], "one");
  EXPECT_EQ(r.records[1], "");
  EXPECT_EQ(r.records[2].size(), 10000u);
  EXPECT_FALSE(r.torn_tail);
}

TEST(Wal, EveryTruncationPointKeepsAPrefixOfRecords) {
  SimFs fs(1);
  {
    LogWriter w(fs.create("/d/1.log"));
    for (int i = 0; i < 5; ++i) w.add("record-" + std::to_string(i));
  }
  const std::string full = fs.contents("/d/1.log");
  for (std::size_t cut = 0; cut <= full.size(); ++cut) {
    fs.create("/d/2.log")->append(full.substr(0, cut));
    const auto r = read_back(fs, "/d/2.log");
    for (std::size_t i = 0; i < r.records.size(); ++i)
      EXPECT_EQ(r.records[i], "record-" + std::to_string(i));
    EXPECT_EQ(r.torn_tail, r.valid_bytes != cut) << "cut " << cut;
  }
}

TEST(Wal, CorruptRecordStopsReading) {
  SimFs fs(1);
  {
    LogWriter w(fs.create("/d/1.log"));
    w.add("aaaa");
    w.add("bbbb");
    w.add("cccc");
  }
  std::string data = fs.contents("/d/1.log");
  data[12 + 8 + 1] ^= 0x01;  // inside the second payload
  fs.create("/d/2.log")->append(data);
  const auto r = read_back(fs, "/d/2.log");
  ASSERT_EQ(r.records.size(), 1u);
  EXPECT_TRUE(r.torn_tail);
}

TEST(WriteBatch, EncodeDecode) {
  WriteBatch b;
  b.put("a", "1");
  b.del("b");
  b.put("c", std::string(300, 'x'));
  SequenceNumber first = 0;
  const WriteBatch d = WriteBatch::decode(b.encode(42), first);
  EXPECT_EQ(first, 42u);
  ASSERT_EQ(d.size(), 3u);
  EXPECT_EQ(d.ops()[1].type, ValueType::Deletion);
  EXPECT_EQ(d.ops()[2].value.size(), 300u);
}
