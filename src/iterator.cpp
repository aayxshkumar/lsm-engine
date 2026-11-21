#include "lsm/iterator.hpp"

namespace lsm {

namespace {

// Linear scan over children to pick the smallest. The number of children is
// small (memtable + a handful of tables), so a heap isn't worth it yet.
class MergingIterator : public InternalIterator {
 public:
  explicit MergingIterator(std::vector<std::unique_ptr<InternalIterator>> c)
      : children_(std::move(c)) {}

  bool valid() const override { return current_ != nullptr; }
  void seek_to_first() override {
    for (auto& c : children_) c->seek_to_first();
    pick();
  }
  void seek(std::string_view key) override {
    for (auto& c : children_) c->seek(key);
    pick();
  }
  void next() override {
    current_->next();
    pick();
  }
  std::string_view key() const override { return current_->key(); }
  SequenceNumber seq() const override { return current_->seq(); }
  ValueType type() const override { return current_->type(); }
  std::string_view value() const override { return current_->value(); }

 private:
  void pick() {
    current_ = nullptr;
    for (auto& c : children_) {
      if (!c->valid()) continue;
      if (!current_ || compare_internal(c->key(), c->seq(), current_->key(), current_->seq()) < 0)
        current_ = c.get();
    }
  }

  std::vector<std::unique_ptr<InternalIterator>> children_;
  InternalIterator* current_ = nullptr;
};

}  // namespace

std::unique_ptr<InternalIterator> make_merging_iterator(
    std::vector<std::unique_ptr<InternalIterator>> children) {
  return std::make_unique<MergingIterator>(std::move(children));
}

}  // namespace lsm
