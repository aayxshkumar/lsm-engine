#include <gtest/gtest.h>

#include "lsm/coding.hpp"
#include "lsm/crc32c.hpp"

using namespace lsm;

TEST(Crc32c, KnownVectors) {
  // From RFC 3720 appendix B.4.
  EXPECT_EQ(crc32c(std::string(32, '\0')), 0x8a9136aau);
  EXPECT_EQ(crc32c(std::string(32, '\xff')), 0x62a8ab43u);
  EXPECT_EQ(crc32c("123456789"), 0xe3069283u);
}

TEST(Crc32c, CanBeChained) {
  EXPECT_EQ(crc32c("456789", crc32c("123")), crc32c("123456789"));
}

TEST(Coding, FixedAndVarintRoundTrip) {
  std::string s;
  put_fixed32(s, 0xdeadbeef);
  put_fixed64(s, 0x0123456789abcdefULL);
  const std::uint64_t vals[] = {0, 1, 127, 128, 300, 1ULL << 35, UINT64_MAX};
  for (auto v : vals) put_varint(s, v);
  put_bytes(s, "hello");
  Decoder d(s);
  EXPECT_EQ(d.fixed32(), 0xdeadbeefu);
  EXPECT_EQ(d.fixed64(), 0x0123456789abcdefULL);
  for (auto v : vals) EXPECT_EQ(d.varint(), v);
  EXPECT_EQ(d.bytes(), "hello");
  EXPECT_TRUE(d.done());
}

TEST(Coding, DecoderRejectsTruncation) {
  std::string s;
  put_bytes(s, "hello");
  s.pop_back();
  Decoder d(s);
  EXPECT_THROW(d.bytes(), Corruption);
  Decoder e(std::string_view("\x80\x80", 2));
  EXPECT_THROW(e.varint(), Corruption);
}
