#include "lsm/bloom.hpp"

#include <algorithm>
#include <cstdint>

namespace lsm {

std::uint64_t hash64(std::string_view data) {
  // FNV-1a, then a murmur3 finaliser to spread the bits.
  std::uint64_t h = 0xcbf29ce484222325ULL;
  for (unsigned char c : data) {
    h ^= c;
    h *= 0x100000001b3ULL;
  }
  h ^= h >> 33;
  h *= 0xff51afd7ed558ccdULL;
  h ^= h >> 33;
  h *= 0xc4ceb9fe1a85ec53ULL;
  h ^= h >> 33;
  return h;
}

std::string BloomFilter::build(const std::vector<std::string>& keys, int bits_per_key) {
  // k = ln 2 * bits/key minimises the false positive rate.
  const int k = std::clamp(static_cast<int>(bits_per_key * 0.69), 1, 30);
  std::size_t bits = std::max<std::size_t>(64, keys.size() * static_cast<std::size_t>(bits_per_key));
  const std::size_t bytes = (bits + 7) / 8;
  bits = bytes * 8;
  std::string filter(bytes, '\0');
  for (const auto& key : keys) {
    const std::uint64_t h = hash64(key);
    std::uint64_t h1 = h & 0xffffffff, h2 = h >> 32;
    for (int i = 0; i < k; ++i) {
      const std::size_t bit = (h1 + static_cast<std::uint64_t>(i) * h2) % bits;
      filter[bit / 8] = static_cast<char>(filter[bit / 8] | (1 << (bit % 8)));
    }
  }
  filter.push_back(static_cast<char>(k));
  return filter;
}

bool BloomFilter::may_contain(std::string_view filter, std::string_view key) {
  if (filter.size() < 2) return true;
  const int k = static_cast<unsigned char>(filter.back());
  const std::size_t bits = (filter.size() - 1) * 8;
  const std::uint64_t h = hash64(key);
  std::uint64_t h1 = h & 0xffffffff, h2 = h >> 32;
  for (int i = 0; i < k; ++i) {
    const std::size_t bit = (h1 + static_cast<std::uint64_t>(i) * h2) % bits;
    if (!(static_cast<unsigned char>(filter[bit / 8]) & (1 << (bit % 8)))) return false;
  }
  return true;
}

}  // namespace lsm
