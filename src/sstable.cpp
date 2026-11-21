#include "lsm/sstable.hpp"

#include <algorithm>

#include "lsm/bloom.hpp"
#include "lsm/coding.hpp"
#include "lsm/crc32c.hpp"

namespace lsm {

namespace {

constexpr std::uint64_t kMagic = 0x6c736d2d74626c31ULL;  // "lsm-tbl1"
constexpr std::size_t kFooterSize = 48;

struct Entry {
  std::string key;
  SequenceNumber seq;
  ValueType type;
  std::string value;
};

std::vector<Entry> decode_block(std::string_view block) {
  std::vector<Entry> out;
  Decoder d(block);
  while (!d.done()) {
    Entry e;
    e.key = std::string(d.bytes());
    e.seq = d.fixed64();
    const auto t = d.u8();
    if (t > 1) throw Corruption("bad entry type in table block");
    e.type = static_cast<ValueType>(t);
    e.value = std::string(d.bytes());
    out.push_back(std::move(e));
  }
  return out;
}

}  // namespace

// ---------------------------------------------------------------- builder

TableBuilder::TableBuilder(std::unique_ptr<WritableFile> file, TableOptions opts)
    : file_(std::move(file)), opts_(opts) {}

void TableBuilder::add(std::string_view key, SequenceNumber seq, ValueType type,
                       std::string_view value) {
  if (entries_ > 0 && key <= last_key_) throw std::logic_error("table keys must strictly increase");
  if (entries_ == 0) meta_.smallest = std::string(key);
  put_bytes(block_, key);
  put_fixed64(block_, seq);
  block_.push_back(static_cast<char>(type));
  put_bytes(block_, value);
  keys_.emplace_back(key);
  last_key_ = std::string(key);
  ++entries_;
  if (block_.size() >= opts_.block_bytes) flush_block();
}

void TableBuilder::write_with_crc(const std::string& payload) {
  std::string rec = payload;
  put_fixed32(rec, crc32c(payload));
  file_->append(rec);
  offset_ += rec.size();
}

void TableBuilder::flush_block() {
  if (block_.empty()) return;
  put_bytes(index_, last_key_);
  put_fixed64(index_, offset_);
  put_fixed32(index_, static_cast<std::uint32_t>(block_.size()));
  write_with_crc(block_);
  block_.clear();
}

TableMeta TableBuilder::finish() {
  flush_block();
  const std::string filter = BloomFilter::build(keys_, opts_.bloom_bits_per_key);
  const std::uint64_t filter_off = offset_;
  write_with_crc(filter);
  const std::uint64_t index_off = offset_;
  write_with_crc(index_);

  std::string footer;
  put_fixed64(footer, index_off);
  put_fixed32(footer, static_cast<std::uint32_t>(index_.size()));
  put_fixed64(footer, filter_off);
  put_fixed32(footer, static_cast<std::uint32_t>(filter.size()));
  put_fixed64(footer, entries_);
  put_fixed64(footer, kMagic);
  put_fixed32(footer, crc32c(footer));
  footer.append(4, '\0');
  file_->append(footer);
  offset_ += footer.size();

  file_->sync();
  file_->close();
  meta_.file_size = offset_;
  meta_.entries = entries_;
  meta_.largest = last_key_;
  return meta_;
}

// ---------------------------------------------------------------- reader

std::shared_ptr<TableReader> TableReader::open(FileSystem& fs, const std::string& path) {
  auto t = std::shared_ptr<TableReader>(new TableReader());
  t->path_ = path;
  t->file_ = fs.open(path);
  const std::uint64_t size = t->file_->size();
  if (size < kFooterSize) throw Corruption(path + ": too short for a table footer");
  const std::string footer = t->file_->read(size - kFooterSize, kFooterSize);
  if (footer.size() != kFooterSize) throw Corruption(path + ": short footer read");
  if (crc32c(std::string_view(footer).substr(0, 40)) != decode_fixed32(footer.data() + 40))
    throw Corruption(path + ": footer checksum mismatch");
  Decoder d(footer);
  const auto index_off = d.fixed64();
  const auto index_size = d.fixed32();
  const auto filter_off = d.fixed64();
  const auto filter_size = d.fixed32();
  t->entries_ = d.fixed64();
  if (d.fixed64() != kMagic) throw Corruption(path + ": bad table magic");

  auto read_checked = [&](std::uint64_t off, std::uint32_t n, const char* what) {
    if (off + n + 4 > size - kFooterSize) throw Corruption(path + ": " + what + " out of range");
    std::string b = t->file_->read(off, n + 4);
    if (b.size() != n + 4) throw Corruption(path + ": short " + what + " read");
    if (crc32c(std::string_view(b).substr(0, n)) != decode_fixed32(b.data() + n))
      throw Corruption(path + ": " + what + " checksum mismatch");
    b.resize(n);
    return b;
  };
  t->filter_ = read_checked(filter_off, filter_size, "filter");
  const std::string index = read_checked(index_off, index_size, "index");
  Decoder id(index);
  while (!id.done()) {
    BlockHandle h;
    h.last_key = std::string(id.bytes());
    h.offset = id.fixed64();
    h.size = id.fixed32();
    t->index_.push_back(std::move(h));
  }
  return t;
}

std::string TableReader::read_block(const BlockHandle& h) const {
  std::string b = file_->read(h.offset, h.size + 4);
  if (b.size() != h.size + 4) throw Corruption(path_ + ": short block read");
  if (crc32c(std::string_view(b).substr(0, h.size)) != decode_fixed32(b.data() + h.size))
    throw Corruption(path_ + ": block checksum mismatch");
  b.resize(h.size);
  return b;
}

std::optional<TableReader::Found> TableReader::get(std::string_view key) const {
  if (!BloomFilter::may_contain(filter_, key)) {
    ++bloom_skips;
    return std::nullopt;
  }
  auto it = std::lower_bound(index_.begin(), index_.end(), key,
                             [](const BlockHandle& h, std::string_view k) { return h.last_key < k; });
  if (it == index_.end()) return std::nullopt;
  for (Entry& e : decode_block(read_block(*it))) {
    if (e.key == key) return Found{e.seq, e.type, std::move(e.value)};
  }
  return std::nullopt;
}

namespace {

class TableIterator : public InternalIterator {
 public:
  explicit TableIterator(const TableReader& t) : t_(t) {}

  bool valid() const override { return block_ < t_.blocks().size() && pos_ < entries_.size(); }
  void seek_to_first() override { load(0); }
  void seek(std::string_view key) override {
    const auto& idx = t_.blocks();
    auto it = std::lower_bound(idx.begin(), idx.end(), key,
                               [](const TableReader::BlockHandle& h, std::string_view k) {
                                 return h.last_key < k;
                               });
    load(static_cast<std::size_t>(it - idx.begin()));
    while (valid() && entries_[pos_].key < key) next();
  }
  void next() override {
    if (++pos_ >= entries_.size()) load(block_ + 1);
  }
  std::string_view key() const override { return entries_[pos_].key; }
  SequenceNumber seq() const override { return entries_[pos_].seq; }
  ValueType type() const override { return entries_[pos_].type; }
  std::string_view value() const override { return entries_[pos_].value; }

 private:
  void load(std::size_t block) {
    block_ = block;
    pos_ = 0;
    entries_.clear();
    while (block_ < t_.blocks().size()) {
      entries_ = decode_block(t_.read_block(t_.blocks()[block_]));
      if (!entries_.empty()) return;
      ++block_;
    }
  }

  const TableReader& t_;
  std::size_t block_ = 0;
  std::size_t pos_ = 0;
  std::vector<Entry> entries_;
};

}  // namespace

std::unique_ptr<InternalIterator> TableReader::iterator() const {
  return std::make_unique<TableIterator>(*this);
}

}  // namespace lsm
