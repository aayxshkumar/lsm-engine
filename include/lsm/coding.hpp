#pragma once

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>

namespace lsm {

// Thrown when on-disk data fails a checksum or doesn't parse.
struct Corruption : std::runtime_error {
  using std::runtime_error::runtime_error;
};

// Little-endian fixed-width and LEB128 varint encoding.

inline void put_fixed32(std::string& dst, std::uint32_t v) {
  char buf[4];
  for (int i = 0; i < 4; ++i) buf[i] = static_cast<char>((v >> (8 * i)) & 0xff);
  dst.append(buf, 4);
}

inline void put_fixed64(std::string& dst, std::uint64_t v) {
  char buf[8];
  for (int i = 0; i < 8; ++i) buf[i] = static_cast<char>((v >> (8 * i)) & 0xff);
  dst.append(buf, 8);
}

inline void put_varint(std::string& dst, std::uint64_t v) {
  while (v >= 0x80) {
    dst.push_back(static_cast<char>((v & 0x7f) | 0x80));
    v >>= 7;
  }
  dst.push_back(static_cast<char>(v));
}

inline void put_bytes(std::string& dst, std::string_view s) {
  put_varint(dst, s.size());
  dst.append(s);
}

inline std::uint32_t decode_fixed32(const char* p) {
  std::uint32_t v = 0;
  for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(static_cast<unsigned char>(p[i])) << (8 * i);
  return v;
}

inline std::uint64_t decode_fixed64(const char* p) {
  std::uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(static_cast<unsigned char>(p[i])) << (8 * i);
  return v;
}

// Bounds-checked reader over a byte string. Every getter throws Corruption
// instead of reading past the end.
class Decoder {
 public:
  explicit Decoder(std::string_view s) : s_(s) {}

  bool done() const { return pos_ >= s_.size(); }
  std::size_t remaining() const { return s_.size() - pos_; }

  std::uint8_t u8() {
    need(1);
    return static_cast<std::uint8_t>(s_[pos_++]);
  }
  std::uint32_t fixed32() {
    need(4);
    auto v = decode_fixed32(s_.data() + pos_);
    pos_ += 4;
    return v;
  }
  std::uint64_t fixed64() {
    need(8);
    auto v = decode_fixed64(s_.data() + pos_);
    pos_ += 8;
    return v;
  }
  std::uint64_t varint() {
    std::uint64_t v = 0;
    for (int shift = 0; shift <= 63; shift += 7) {
      need(1);
      const auto b = static_cast<unsigned char>(s_[pos_++]);
      v |= static_cast<std::uint64_t>(b & 0x7f) << shift;
      if (!(b & 0x80)) return v;
    }
    throw Corruption("varint too long");
  }
  std::string_view bytes() {
    const auto n = varint();
    return raw(n);
  }
  std::string_view raw(std::uint64_t n) {
    need(n);
    auto v = s_.substr(pos_, n);
    pos_ += n;
    return v;
  }

 private:
  void need(std::uint64_t n) const {
    if (n > s_.size() - pos_) throw Corruption("truncated record");
  }
  std::string_view s_;
  std::size_t pos_ = 0;
};

}  // namespace lsm
