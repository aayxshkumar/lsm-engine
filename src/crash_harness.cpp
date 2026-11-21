#include "lsm/crash_harness.hpp"

#include <cstdio>
#include <map>
#include <optional>
#include <vector>

#include "lsm/sim_fs.hpp"

namespace lsm {

namespace {

using Model = std::map<std::string, std::string>;

void apply_batch(Model& m, const WriteBatch& b) {
  for (const auto& op : b.ops()) {
    if (op.type == ValueType::Value) m[op.key] = op.value;
    else m.erase(op.key);
  }
}

std::string show(const std::optional<std::string>& v) {
  if (!v) return "<absent>";
  return v->size() > 40 ? "'" + v->substr(0, 40) + "...'" : "'" + *v + "'";
}

class Run {
 public:
  explicit Run(const CrashConfig& cfg) : cfg_(cfg), fs_(cfg.seed * 0x9e3779b97f4a7c15ULL + 1), rng_(cfg.seed) {}

  CrashResult go() {
    try {
      for (int round = 0; round < cfg_.rounds && r_.failure.empty(); ++round) {
        if (!open_and_verify(round > 0)) break;
        const bool last = round + 1 == cfg_.rounds;
        if (!last) fs_.arm_crash(rng_.uniform(1, static_cast<std::uint64_t>(cfg_.ops_per_round) * 3));
        const bool crashed = work();
        totals();
        db_.reset();
        fs_.disarm();
        // Power loss whether or not the crash point fired: unsynced data in
        // every file is at risk either way.
        fs_.crash();
        if (crashed) ++r_.crashes;
      }
      if (r_.failure.empty()) open_and_verify(true);
    } catch (const SimCrash&) {
      fail("SimCrash escaped the harness (crash point hit outside a guarded call)");
    } catch (const std::exception& e) {
      fail(std::string("exception: ") + e.what());
    }
    r_.fs_ops = fs_.mutations();
    return r_;
  }

 private:
  void fail(std::string what) {
    if (r_.failure.empty()) r_.failure = std::move(what);
  }

  void totals() {
    if (!db_) return;
    const DbStats s = db_->stats();
    r_.flushes += s.flushes - flushes_at_open_;
    r_.compactions += s.compactions - compactions_at_open_;
  }

  // Opens the DB (possibly crashing during recovery and retrying), then
  // checks the recovered state.
  bool open_and_verify(bool may_crash_in_open) {
    while (true) {
      if (may_crash_in_open && rng_.chance(cfg_.crash_in_open_prob))
        fs_.arm_crash(rng_.uniform(1, 12));
      try {
        db_ = DB::open(fs_, "/db", cfg_.opts);
        fs_.disarm();
        break;
      } catch (const SimCrash&) {
        db_.reset();
        fs_.crash();
        ++r_.crashes_in_open;
      }
    }
    const DbStats s = db_->stats();
    if (s.recovered_torn_log) ++r_.torn_logs;
    flushes_at_open_ = s.flushes;
    compactions_at_open_ = s.compactions;
    return verify_recovery();
  }

  bool verify_recovery() {
    // Which prefix of the pending batches did recovery keep?
    const SequenceNumber got = db_->last_sequence();
    SequenceNumber seq = base_seq_;
    std::size_t keep = SIZE_MAX;
    for (std::size_t j = 0; j <= pending_.size(); ++j) {
      if (seq == got) {
        keep = j;
        break;
      }
      if (j < pending_.size()) seq += pending_[j].size();
    }
    if (keep == SIZE_MAX) {
      fail("recovered last_sequence " + std::to_string(got) + " is not a batch boundary in [" +
           std::to_string(base_seq_) + ", " + std::to_string(seq) +
           "] (durable writes lost, or a batch half-applied)");
      return false;
    }
    Model expect = base_;
    for (std::size_t j = 0; j < keep; ++j) apply_batch(expect, pending_[j]);

    const auto rows = db_->scan("");
    Model actual(rows.begin(), rows.end());
    if (actual != expect) {
      std::string diff;
      for (const auto& [k, v] : expect) {
        auto it = actual.find(k);
        if (it == actual.end() || it->second != v) {
          diff = "key " + k + ": expected " + show(v) + ", got " +
                 show(it == actual.end() ? std::nullopt : std::optional<std::string>(it->second));
          break;
        }
      }
      if (diff.empty()) {
        for (const auto& [k, v] : actual)
          if (!expect.count(k)) {
            diff = "key " + k + ": expected <absent>, got " + show(v);
            break;
          }
      }
      fail("after recovery (kept " + std::to_string(keep) + " of " +
           std::to_string(pending_.size()) + " unsynced batches): " + diff);
      return false;
    }
    // Spot-check point reads agree with the scan.
    for (int i = 0; i < 8; ++i) {
      const std::string k = key(rng_.uniform(0, static_cast<std::uint64_t>(cfg_.keys - 1)));
      auto it = expect.find(k);
      const auto want = it == expect.end() ? std::nullopt : std::optional<std::string>(it->second);
      const auto have = db_->get(k);
      if (have != want) {
        fail("after recovery, get(" + k + ") returned " + show(have) + " but scan says " + show(want));
        return false;
      }
    }
    // A successful open wrote everything it recovered to a synced table.
    base_ = std::move(expect);
    base_seq_ = got;
    current_ = base_;
    pending_.clear();
    return true;
  }

  std::string key(std::uint64_t i) const {
    char buf[16];
    std::snprintf(buf, sizeof buf, "key%05llu", static_cast<unsigned long long>(i));
    return buf;
  }

  std::string value() {
    std::string v = "v" + std::to_string(++value_counter_) + ":";
    const std::size_t n = rng_.uniform(0, cfg_.max_value);
    for (std::size_t i = 0; i < n; ++i) v.push_back(static_cast<char>('a' + rng_.uniform(0, 25)));
    return v;
  }

  // Returns true if the armed crash fired.
  bool work() {
    for (int i = 0; i < cfg_.ops_per_round; ++i) {
      if (rng_.chance(cfg_.read_prob)) {
        const std::string k = key(rng_.uniform(0, static_cast<std::uint64_t>(cfg_.keys - 1)));
        auto it = current_.find(k);
        const auto want = it == current_.end() ? std::nullopt : std::optional<std::string>(it->second);
        const auto have = db_->get(k);
        ++r_.reads_checked;
        if (have != want) {
          fail("get(" + k + ") returned " + show(have) + ", expected " + show(want));
          return false;
        }
        continue;
      }
      WriteBatch b;
      const int n = rng_.chance(cfg_.batch_prob) ? static_cast<int>(rng_.uniform(2, 5)) : 1;
      for (int j = 0; j < n; ++j) {
        std::string k = key(rng_.uniform(0, static_cast<std::uint64_t>(cfg_.keys - 1)));
        if (rng_.chance(cfg_.delete_prob)) b.del(std::move(k));
        else b.put(std::move(k), value());
      }
      const bool sync = rng_.chance(cfg_.sync_prob);
      const auto flushes_before = db_->stats().flushes;
      try {
        db_->write(b, WriteOptions{sync});
      } catch (const SimCrash&) {
        pending_.push_back(std::move(b));  // may or may not have happened
        return true;
      }
      ++r_.writes_acked;
      apply_batch(current_, b);
      pending_.push_back(std::move(b));
      if (sync || db_->stats().flushes != flushes_before) {
        // Everything acknowledged so far is durable.
        for (const auto& p : pending_) {
          apply_batch(base_, p);
          base_seq_ += p.size();
        }
        pending_.clear();
      }
    }
    return false;
  }

  const CrashConfig& cfg_;
  SimFs fs_;
  Rng rng_;
  std::unique_ptr<DB> db_;
  CrashResult r_;

  Model base_;                       // state after every known-durable batch
  SequenceNumber base_seq_ = 0;
  std::vector<WriteBatch> pending_;  // batches after base_, in order
  Model current_;                    // base_ + all acknowledged pending_
  std::uint64_t value_counter_ = 0;
  std::uint64_t flushes_at_open_ = 0;
  std::uint64_t compactions_at_open_ = 0;
};

}  // namespace

CrashResult run_crash_test(const CrashConfig& cfg) { return Run(cfg).go(); }

}  // namespace lsm
