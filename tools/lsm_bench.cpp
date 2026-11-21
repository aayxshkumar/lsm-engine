// lsm_bench: single-threaded throughput on the real file system.
//
//   lsm_bench [--dir PATH] [--n N] [--value BYTES] [--sync-every K]
//
// Workloads, in order, on one database:
//   fillseq      N puts, keys in order
//   fillrandom   N puts, random keys (overwrites)
//   readrandom   N gets of existing keys
//   readmissing  N gets of keys that were never written (bloom filter path)
//   scan         full scan
// Then reports write amplification (bytes the engine wrote / user bytes).

#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "lsm/db.hpp"
#include "lsm/rng.hpp"

using namespace lsm;
using Clock = std::chrono::steady_clock;

namespace {

std::string key(std::uint64_t i) {
  char buf[24];
  std::snprintf(buf, sizeof buf, "user%012llu", static_cast<unsigned long long>(i));
  return buf;
}

void report(const char* name, std::uint64_t n, Clock::time_point t0) {
  const double s = std::chrono::duration<double>(Clock::now() - t0).count();
  std::printf("%-12s %9llu ops %8.3f s %12.0f ops/s %8.2f us/op\n", name,
              static_cast<unsigned long long>(n), s, static_cast<double>(n) / s, s * 1e6 / static_cast<double>(n));
}

}  // namespace

int main(int argc, char** argv) {
  std::string dir = "/tmp/lsm_bench_" + std::to_string(::getpid());
  std::uint64_t n = 200000, value_size = 100, sync_every = 0;
  for (int i = 1; i + 1 < argc; i += 2) {
    if (!std::strcmp(argv[i], "--dir")) dir = argv[i + 1];
    else if (!std::strcmp(argv[i], "--n")) n = std::strtoull(argv[i + 1], nullptr, 10);
    else if (!std::strcmp(argv[i], "--value")) value_size = std::strtoull(argv[i + 1], nullptr, 10);
    else if (!std::strcmp(argv[i], "--sync-every")) sync_every = std::strtoull(argv[i + 1], nullptr, 10);
    else {
      std::fprintf(stderr, "unknown flag %s\n", argv[i]);
      return 2;
    }
  }

  PosixFs fs;
  fs.mkdir(dir);
  for (const auto& f : fs.list(dir)) fs.remove(dir + "/" + f);
  auto db = DB::open(fs, dir);
  Rng rng(42);
  const std::string value(value_size, 'x');
  std::printf("dir %s, n %llu, value %llu B, sync every %llu\n", dir.c_str(),
              static_cast<unsigned long long>(n), static_cast<unsigned long long>(value_size),
              static_cast<unsigned long long>(sync_every));

  auto wo = [&](std::uint64_t i) { return WriteOptions{sync_every && (i + 1) % sync_every == 0}; };

  auto t0 = Clock::now();
  for (std::uint64_t i = 0; i < n; ++i) db->put(key(i), value, wo(i));
  report("fillseq", n, t0);

  t0 = Clock::now();
  for (std::uint64_t i = 0; i < n; ++i) db->put(key(rng.uniform(0, n - 1)), value, wo(i));
  report("fillrandom", n, t0);

  t0 = Clock::now();
  std::uint64_t found = 0;
  for (std::uint64_t i = 0; i < n; ++i) found += db->get(key(rng.uniform(0, n - 1))).has_value();
  report("readrandom", n, t0);
  if (found != n) std::printf("  WARNING: %llu of %llu keys missing\n",
                              static_cast<unsigned long long>(n - found), static_cast<unsigned long long>(n));

  t0 = Clock::now();
  for (std::uint64_t i = 0; i < n; ++i) found += db->get(key(n + rng.uniform(0, n - 1))).has_value();
  report("readmissing", n, t0);

  t0 = Clock::now();
  const auto rows = db->scan("");
  report("scan", rows.size(), t0);

  const DbStats s = db->stats();
  std::printf("flushes %llu, compactions %llu, L0 %zu, L1 %zu, bloom skips %llu\n",
              static_cast<unsigned long long>(s.flushes),
              static_cast<unsigned long long>(s.compactions), s.l0_files, s.l1_files,
              static_cast<unsigned long long>(s.bloom_skips));
  std::printf("write amplification %.2f (%llu bytes written for %llu user bytes)\n",
              static_cast<double>(fs.stats().bytes_written) / static_cast<double>(s.user_bytes_written),
              static_cast<unsigned long long>(fs.stats().bytes_written),
              static_cast<unsigned long long>(s.user_bytes_written));
  db.reset();
  for (const auto& f : fs.list(dir)) fs.remove(dir + "/" + f);
  return 0;
}
