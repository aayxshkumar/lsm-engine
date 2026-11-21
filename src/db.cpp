#include "lsm/db.hpp"

#include <algorithm>
#include <cstdio>
#include <set>

#include "lsm/coding.hpp"

namespace lsm {

namespace {

// Parses "000123.log" / "000123.sst". Returns false for anything else.
bool parse_name(const std::string& name, std::uint64_t& number, std::string& ext) {
  const auto dot = name.find('.');
  if (dot == std::string::npos || dot == 0) return false;
  for (std::size_t i = 0; i < dot; ++i)
    if (name[i] < '0' || name[i] > '9') return false;
  number = std::stoull(name.substr(0, dot));
  ext = name.substr(dot + 1);
  return ext == "log" || ext == "sst";
}

}  // namespace

DB::DB(FileSystem& fs, std::string dir, Options opts)
    : fs_(fs), dir_(std::move(dir)), opts_(opts) {}

DB::~DB() = default;

std::unique_ptr<DB> DB::open(FileSystem& fs, std::string dir, Options opts) {
  std::unique_ptr<DB> db(new DB(fs, std::move(dir), opts));
  db->recover();
  return db;
}

std::string DB::path(std::uint64_t number, const char* ext) const {
  char buf[32];
  std::snprintf(buf, sizeof buf, "/%06llu.%s", static_cast<unsigned long long>(number), ext);
  return dir_ + buf;
}

std::shared_ptr<TableReader> DB::table(std::uint64_t number) {
  auto it = tables_.find(number);
  if (it != tables_.end()) return it->second;
  auto t = TableReader::open(fs_, path(number, "sst"));
  tables_[number] = t;
  return t;
}

// ---------------------------------------------------------------- recovery

void DB::recover() {
  fs_.mkdir(dir_);
  read_manifest(fs_, dir_, manifest_);  // leaves the defaults for a new DB
  last_seq_ = manifest_.last_sequence;

  // Logs at or after log_number hold writes not yet in any table. Replay
  // them in file order.
  std::vector<std::uint64_t> logs;
  std::uint64_t max_number = 0;
  for (const auto& name : fs_.list(dir_)) {
    std::uint64_t n;
    std::string ext;
    if (!parse_name(name, n, ext)) continue;
    max_number = std::max(max_number, n);
    if (ext == "log" && n >= manifest_.log_number) logs.push_back(n);
  }
  manifest_.next_file = std::max(manifest_.next_file, max_number + 1);
  std::sort(logs.begin(), logs.end());
  for (std::uint64_t n : logs) {
    auto f = fs_.open(path(n, "log"));
    const LogReadResult r = read_log(*f);
    if (r.torn_tail) stats_.recovered_torn_log = true;
    for (const auto& rec : r.records) {
      SequenceNumber first = 0;
      // A record that passed its checksum but doesn't parse is real
      // corruption, not a torn write: let Corruption propagate.
      const WriteBatch b = WriteBatch::decode(rec, first);
      for (std::size_t i = 0; i < b.size(); ++i) {
        const auto& op = b.ops()[i];
        mem_.add(first + i, op.type, op.key, op.value);
      }
      if (!b.empty()) last_seq_ = std::max(last_seq_, first + b.size() - 1);
    }
  }

  // Never append to a replayed log: its tail may be torn, and anything
  // written after a torn record would be unreadable. Start a fresh one.
  log_file_ = manifest_.next_file++;
  log_ = std::make_unique<LogWriter>(fs_.create(path(log_file_, "log")));

  Manifest next = manifest_;
  if (!mem_.empty()) {
    // Write the recovered memtable straight to L0, so the old logs can go.
    const std::uint64_t number = next.next_file++;
    TableBuilder b(fs_.create(path(number, "sst")), opts_.table);
    auto it = mem_.iterator();
    std::string last;
    bool first = true;
    for (it->seek_to_first(); it->valid(); it->next()) {
      if (!first && it->key() == last) continue;  // older version
      first = false;
      last = std::string(it->key());
      b.add(it->key(), it->seq(), it->type(), it->value());
    }
    const TableMeta tm = b.finish();
    next.files.push_back(FileMeta{0, number, tm.file_size, tm.smallest, tm.largest});
    ++stats_.flushes;
  }
  next.log_number = log_file_;
  next.last_sequence = last_seq_;
  apply_manifest(std::move(next));
  mem_ = MemTable{};
  delete_obsolete_files();
  maybe_compact();
}

void DB::apply_manifest(Manifest next) {
  write_manifest(fs_, dir_, next);
  manifest_ = std::move(next);
}

void DB::delete_obsolete_files() {
  std::set<std::uint64_t> live;
  for (const auto& f : manifest_.files) live.insert(f.number);
  for (const auto& name : fs_.list(dir_)) {
    if (name == "MANIFEST.tmp") {
      fs_.remove(dir_ + "/" + name);
      continue;
    }
    std::uint64_t n;
    std::string ext;
    if (!parse_name(name, n, ext)) continue;
    const bool obsolete = ext == "log" ? n < manifest_.log_number : live.count(n) == 0;
    if (obsolete) {
      tables_.erase(n);
      fs_.remove(dir_ + "/" + name);
    }
  }
}

// ---------------------------------------------------------------- writes

void DB::put(std::string_view key, std::string_view value, WriteOptions wo) {
  WriteBatch b;
  b.put(std::string(key), std::string(value));
  write(b, wo);
}

void DB::del(std::string_view key, WriteOptions wo) {
  WriteBatch b;
  b.del(std::string(key));
  write(b, wo);
}

void DB::write(const WriteBatch& batch, WriteOptions wo) {
  if (batch.empty()) return;
  const SequenceNumber first = last_seq_ + 1;
  log_->add(batch.encode(first));
  if (wo.sync) log_->sync();
  for (std::size_t i = 0; i < batch.size(); ++i) {
    const auto& op = batch.ops()[i];
    mem_.add(first + i, op.type, op.key, op.value);
    stats_.user_bytes_written += op.key.size() + op.value.size();
  }
  last_seq_ += batch.size();
  if (mem_.approximate_bytes() >= opts_.memtable_bytes) flush();
}

void DB::flush() {
  if (mem_.empty()) return;
  // New writes go to a new log; the old one can be deleted once the
  // memtable is safely in a table and the manifest says so.
  const std::uint64_t new_log = manifest_.next_file++;
  auto new_writer = std::make_unique<LogWriter>(fs_.create(path(new_log, "log")));

  const std::uint64_t number = manifest_.next_file++;
  TableBuilder b(fs_.create(path(number, "sst")), opts_.table);
  auto it = mem_.iterator();
  std::string last;
  bool first = true;
  for (it->seek_to_first(); it->valid(); it->next()) {
    if (!first && it->key() == last) continue;  // older version of the same key
    first = false;
    last = std::string(it->key());
    // Deletions are kept: they must hide older values in other tables.
    b.add(it->key(), it->seq(), it->type(), it->value());
  }
  const TableMeta tm = b.finish();

  Manifest next = manifest_;
  next.files.push_back(FileMeta{0, number, tm.file_size, tm.smallest, tm.largest});
  next.log_number = new_log;
  next.last_sequence = last_seq_;
  apply_manifest(std::move(next));

  log_->close();
  const std::uint64_t old_log = log_file_;
  log_ = std::move(new_writer);
  log_file_ = new_log;
  mem_ = MemTable{};
  ++stats_.flushes;
  fs_.remove(path(old_log, "log"));
  maybe_compact();
}

void DB::maybe_compact() {
  std::size_t l0 = 0;
  for (const auto& f : manifest_.files)
    if (f.level == 0) ++l0;
  if (static_cast<int>(l0) >= opts_.l0_compaction_trigger) compact();
}

void DB::compact() {
  std::vector<FileMeta> inputs = manifest_.files;
  if (std::none_of(inputs.begin(), inputs.end(), [](const FileMeta& f) { return f.level == 0; }))
    return;

  std::vector<std::shared_ptr<TableReader>> readers;
  std::vector<std::unique_ptr<InternalIterator>> children;
  for (const auto& f : inputs) {
    readers.push_back(table(f.number));
    children.push_back(readers.back()->iterator());
  }
  auto merged = make_merging_iterator(std::move(children));

  Manifest next = manifest_;
  next.files.clear();
  std::unique_ptr<TableBuilder> out;
  std::uint64_t out_number = 0;
  auto finish_output = [&] {
    if (!out) return;
    const TableMeta tm = out->finish();
    next.files.push_back(FileMeta{1, out_number, tm.file_size, tm.smallest, tm.largest});
    out.reset();
  };

  std::string last;
  bool first = true;
  for (merged->seek_to_first(); merged->valid(); merged->next()) {
    if (!first && merged->key() == last) continue;  // shadowed by a newer version
    first = false;
    last = std::string(merged->key());
    // L1 is the bottom level, so nothing older can be hiding under a
    // deletion: drop it.
    if (merged->type() == ValueType::Deletion) continue;
    if (!out) {
      out_number = next.next_file++;
      out = std::make_unique<TableBuilder>(fs_.create(path(out_number, "sst")), opts_.table);
    }
    out->add(merged->key(), merged->seq(), merged->type(), merged->value());
    if (out->estimated_size() >= opts_.target_file_bytes) finish_output();
  }
  finish_output();
  manifest_.next_file = next.next_file;

  next.last_sequence = last_seq_;
  apply_manifest(std::move(next));
  ++stats_.compactions;
  readers.clear();
  for (const auto& f : inputs) {
    tables_.erase(f.number);
    fs_.remove(path(f.number, "sst"));
  }
}

// ---------------------------------------------------------------- reads

std::optional<std::string> DB::get(std::string_view key) {
  if (auto m = mem_.get(key)) {
    if (m->type == ValueType::Deletion) return std::nullopt;
    return m->value;
  }
  // L0: newest table first.
  for (auto it = manifest_.files.rbegin(); it != manifest_.files.rend(); ++it) {
    if (it->level != 0 || key < it->smallest || key > it->largest) continue;
    if (auto f = table(it->number)->get(key)) {
      if (f->type == ValueType::Deletion) return std::nullopt;
      return f->value;
    }
  }
  // L1: disjoint and sorted, so at most one table can hold the key.
  for (const auto& f : manifest_.files) {
    if (f.level != 1 || key < f.smallest || key > f.largest) continue;
    if (auto found = table(f.number)->get(key)) {
      if (found->type == ValueType::Deletion) return std::nullopt;
      return found->value;
    }
    break;
  }
  return std::nullopt;
}

std::vector<std::pair<std::string, std::string>> DB::scan(std::string_view start,
                                                           std::string_view end,
                                                           std::size_t limit) {
  std::vector<std::shared_ptr<TableReader>> readers;
  std::vector<std::unique_ptr<InternalIterator>> children;
  children.push_back(mem_.iterator());
  for (const auto& f : manifest_.files) {
    readers.push_back(table(f.number));
    children.push_back(readers.back()->iterator());
  }
  auto it = make_merging_iterator(std::move(children));
  std::vector<std::pair<std::string, std::string>> out;
  std::string last;
  bool first = true;
  for (it->seek(start); it->valid() && out.size() < limit; it->next()) {
    if (!end.empty() && it->key() >= end) break;
    if (!first && it->key() == last) continue;
    first = false;
    last = std::string(it->key());
    if (it->type() == ValueType::Deletion) continue;
    out.emplace_back(std::string(it->key()), std::string(it->value()));
  }
  return out;
}

DbStats DB::stats() const {
  DbStats s = stats_;
  for (const auto& f : manifest_.files) (f.level == 0 ? s.l0_files : s.l1_files)++;
  s.memtable_entries = mem_.entries();
  for (const auto& [n, t] : tables_) s.bloom_skips += t->bloom_skips;
  return s;
}

}  // namespace lsm
