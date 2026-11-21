#pragma once

#include <memory>
#include <string_view>
#include <vector>

#include "lsm/format.hpp"

namespace lsm {

// Iterator over internal entries (user key, sequence, type, value) in
// internal-key order. Used for flushes, compactions and scans.
class InternalIterator {
 public:
  virtual ~InternalIterator() = default;
  virtual bool valid() const = 0;
  virtual void seek_to_first() = 0;
  // Positions at the first entry whose user key is >= key.
  virtual void seek(std::string_view key) = 0;
  virtual void next() = 0;
  virtual std::string_view key() const = 0;
  virtual SequenceNumber seq() const = 0;
  virtual ValueType type() const = 0;
  virtual std::string_view value() const = 0;
};

// Merges children into one stream in internal-key order. Children must not
// contain the same (key, sequence) twice.
std::unique_ptr<InternalIterator> make_merging_iterator(
    std::vector<std::unique_ptr<InternalIterator>> children);

}  // namespace lsm
