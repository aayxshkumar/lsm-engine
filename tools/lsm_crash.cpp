// lsm_crash: seeded crash-recovery sweeps on the simulated file system.
//
//   lsm_crash --seed 7            one run, with a summary
//   lsm_crash --seeds 500         seeds 1..500, one line per failure
//
// Exit code: 0 if every seed passed, 1 if any failed, 2 on bad arguments.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "lsm/crash_harness.hpp"

using namespace lsm;

int main(int argc, char** argv) {
  CrashConfig base;
  std::uint64_t first = 1, count = 1;
  bool single = true, quiet = false;
  for (int i = 1; i < argc; ++i) {
    const char* a = argv[i];
    auto num = [&]() -> std::uint64_t {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "missing value for %s\n", a);
        std::exit(2);
      }
      return std::strtoull(argv[++i], nullptr, 10);
    };
    if (!std::strcmp(a, "--seed")) {
      first = num();
      count = 1;
      single = true;
    } else if (!std::strcmp(a, "--seeds")) {
      count = num();
      single = false;
    } else if (!std::strcmp(a, "--first")) {
      first = num();
    } else if (!std::strcmp(a, "--rounds")) {
      base.rounds = static_cast<int>(num());
    } else if (!std::strcmp(a, "--ops")) {
      base.ops_per_round = static_cast<int>(num());
    } else if (!std::strcmp(a, "--keys")) {
      base.keys = static_cast<int>(num());
    } else if (!std::strcmp(a, "--quiet")) {
      quiet = true;
    } else {
      std::fprintf(stderr,
                   "usage: lsm_crash [--seed N | --seeds N [--first N]] [--rounds N] [--ops N]\n"
                   "                 [--keys N] [--quiet]\n");
      return 2;
    }
  }
  if (count == 0 || base.keys < 1 || base.rounds < 1) return 2;

  const auto t0 = std::chrono::steady_clock::now();
  std::uint64_t failed = 0;
  for (std::uint64_t seed = first; seed < first + count; ++seed) {
    CrashConfig cfg = base;
    cfg.seed = seed;
    const CrashResult r = run_crash_test(cfg);
    if (!r.ok()) ++failed;
    if (single) {
      std::printf(
          "seed %llu: %s\n"
          "  %d crashes (%d during recovery), %d recoveries found a torn WAL tail\n"
          "  %llu writes acknowledged, %llu reads checked\n"
          "  %llu flushes, %llu compactions, %llu file-system mutations\n",
          static_cast<unsigned long long>(seed), r.ok() ? "PASS" : "FAIL", r.crashes,
          r.crashes_in_open, r.torn_logs, static_cast<unsigned long long>(r.writes_acked),
          static_cast<unsigned long long>(r.reads_checked),
          static_cast<unsigned long long>(r.flushes),
          static_cast<unsigned long long>(r.compactions),
          static_cast<unsigned long long>(r.fs_ops));
      if (!r.ok()) std::printf("  FAILURE: %s\n", r.failure.c_str());
    } else if (!r.ok()) {
      std::printf("seed %llu FAIL: %s\n", static_cast<unsigned long long>(seed), r.failure.c_str());
    } else if (!quiet && seed % 100 == 0) {
      std::fprintf(stderr, "... %llu\n", static_cast<unsigned long long>(seed));
    }
  }
  const double wall =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  if (!single)
    std::printf("%llu/%llu seeds passed (%.1f s wall)\n",
                static_cast<unsigned long long>(count - failed),
                static_cast<unsigned long long>(count), wall);
  return failed ? 1 : 0;
}
