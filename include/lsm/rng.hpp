#pragma once

#include <cstdint>

namespace lsm {

// xoshiro256** seeded through splitmix64. The std distributions differ
// between standard libraries, so all test randomness goes through this to
// make a seed mean the same run everywhere.
class Rng {
 public:
  explicit Rng(std::uint64_t seed) {
    std::uint64_t x = seed;
    for (auto& w : s_) w = splitmix64(x);
  }

  std::uint64_t next() {
    const std::uint64_t result = rotl(s_[1] * 5, 7) * 9;
    const std::uint64_t t = s_[1] << 17;
    s_[2] ^= s_[0];
    s_[3] ^= s_[1];
    s_[1] ^= s_[2];
    s_[0] ^= s_[3];
    s_[2] ^= t;
    s_[3] = rotl(s_[3], 45);
    return result;
  }

  // Uniform in [lo, hi], inclusive.
  std::uint64_t uniform(std::uint64_t lo, std::uint64_t hi) {
    if (hi <= lo) return lo;
    const std::uint64_t span = hi - lo + 1;
    if (span == 0) return next();
    const std::uint64_t limit = UINT64_MAX - (UINT64_MAX % span);
    std::uint64_t x;
    do {
      x = next();
    } while (x >= limit);
    return lo + x % span;
  }

  double unit() { return static_cast<double>(next() >> 11) * 0x1.0p-53; }
  bool chance(double p) { return unit() < p; }

 private:
  static std::uint64_t rotl(std::uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }
  static std::uint64_t splitmix64(std::uint64_t& x) {
    std::uint64_t z = (x += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
  }
  std::uint64_t s_[4];
};

}  // namespace lsm
