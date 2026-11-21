#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "lsm/format.hpp"
#include "lsm/iterator.hpp"

namespace lsm {

// In-memory write buffer holding every version written since the last flush.
// A std::map for now; a concurrent skiplist is on the roadmap.
class MemTable {
 public:
  struct Found {
    ValueType type;
    std::string value;
  };

  void add(SequenceNumber seq, ValueType type, std::string_view key, std::string_view value);
  // Newest version of key, if the memtable has one (a Deletion counts).
  std::optional<Found> get(std::string_view key) const;

  bool empty() const { return table_.empty(); }
  std::size_t entries() const { return table_.size(); }
  // Approximate bytes used, for the flush trigger.
  std::size_t approximate_bytes() const { return bytes_; }

  std::unique_ptr<InternalIterator> iterator() const;

  struct Key {
    std::string user_key;
    SequenceNumber seq;
  };
  struct KeyLess {
    using is_transparent = void;
    bool operator()(const Key& a, const Key& b) const {
      return compare_internal(a.user_key, a.seq, b.user_key, b.seq) < 0;
    }
  };
  using Table = std::map<Key, Found, KeyLess>;

 private:
  Table table_;
  std::size_t bytes_ = 0;
};

}  // namespace lsm
