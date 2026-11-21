#include "lsm/memtable.hpp"

namespace lsm {

void MemTable::add(SequenceNumber seq, ValueType type, std::string_view key,
                   std::string_view value) {
  table_.emplace(Key{std::string(key), seq}, Found{type, std::string(value)});
  bytes_ += key.size() + value.size() + 48;  // rough map node overhead
}

std::optional<MemTable::Found> MemTable::get(std::string_view key) const {
  auto it = table_.lower_bound(Key{std::string(key), kMaxSequence});
  if (it == table_.end() || it->first.user_key != key) return std::nullopt;
  return it->second;
}

namespace {

class MemTableIterator : public InternalIterator {
 public:
  explicit MemTableIterator(const MemTable::Table& t) : t_(t), it_(t.end()) {}
  bool valid() const override { return it_ != t_.end(); }
  void seek_to_first() override { it_ = t_.begin(); }
  void seek(std::string_view key) override {
    it_ = t_.lower_bound(MemTable::Key{std::string(key), kMaxSequence});
  }
  void next() override { ++it_; }
  std::string_view key() const override { return it_->first.user_key; }
  SequenceNumber seq() const override { return it_->first.seq; }
  ValueType type() const override { return it_->second.type; }
  std::string_view value() const override { return it_->second.value; }

 private:
  const MemTable::Table& t_;
  MemTable::Table::const_iterator it_;
};

}  // namespace

std::unique_ptr<InternalIterator> MemTable::iterator() const {
  return std::make_unique<MemTableIterator>(table_);
}

}  // namespace lsm
