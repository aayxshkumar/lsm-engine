#include "lsm/crc32c.hpp"

#include <array>

namespace lsm {

namespace {

constexpr std::array<std::uint32_t, 256> make_table() {
  std::array<std::uint32_t, 256> t{};
  for (std::uint32_t i = 0; i < 256; ++i) {
    std::uint32_t c = i;
    for (int k = 0; k < 8; ++k) c = (c & 1) ? (c >> 1) ^ 0x82f63b78u : c >> 1;
    t[i] = c;
  }
  return t;
}

constexpr auto kTable = make_table();

}  // namespace

std::uint32_t crc32c(std::string_view data, std::uint32_t crc) {
  crc = ~crc;
  for (unsigned char b : data) crc = kTable[(crc ^ b) & 0xff] ^ (crc >> 8);
  return ~crc;
}

}  // namespace lsm
