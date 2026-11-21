#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace lsm {

// Bloom filter over a table's keys, using double hashing (Kirsch &
// Mitzenmacher) from one 64-bit hash. The encoded filter is the bit array
// followed by one byte holding the number of probes.
class BloomFilter {
 public:
  static std::string build(const std::vector<std::string>& keys, int bits_per_key);
  static bool may_contain(std::string_view filter, std::string_view key);
};

std::uint64_t hash64(std::string_view data);

}  // namespace lsm
