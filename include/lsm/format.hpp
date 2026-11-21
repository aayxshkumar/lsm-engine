#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace lsm {

using SequenceNumber = std::uint64_t;
constexpr SequenceNumber kMaxSequence = UINT64_MAX;

enum class ValueType : std::uint8_t { Deletion = 0, Value = 1 };

// Internal keys order by user key ascending, then sequence descending, so the
// newest version of a key comes first.
inline int compare_internal(std::string_view a, SequenceNumber sa, std::string_view b,
                            SequenceNumber sb) {
  if (const int c = a.compare(b); c != 0) return c < 0 ? -1 : 1;
  if (sa == sb) return 0;
  return sa > sb ? -1 : 1;
}

// A group of writes applied atomically with consecutive sequence numbers.
class WriteBatch {
 public:
  struct Op {
    ValueType type;
    std::string key;
    std::string value;
  };

  void put(std::string key, std::string value) {
    ops_.push_back({ValueType::Value, std::move(key), std::move(value)});
  }
  void del(std::string key) { ops_.push_back({ValueType::Deletion, std::move(key), {}}); }
  void clear() { ops_.clear(); }

  const std::vector<Op>& ops() const { return ops_; }
  std::size_t size() const { return ops_.size(); }
  bool empty() const { return ops_.empty(); }

  // WAL payload: fixed64 first_seq, fixed32 count, then per op
  // u8 type, bytes key, bytes value.
  std::string encode(SequenceNumber first_seq) const;
  static WriteBatch decode(std::string_view payload, SequenceNumber& first_seq);

 private:
  std::vector<Op> ops_;
};

}  // namespace lsm
