#include "lsm/sim_fs.hpp"

#include <stdexcept>

namespace lsm {

class SimWritable : public WritableFile {
 public:
  SimWritable(SimFs& fs, std::shared_ptr<SimFs::File> f) : fs_(fs), f_(std::move(f)) {}

  void append(std::string_view data) override {
    if (fs_.crash_at_ != 0 && fs_.mutations_ + 1 == fs_.crash_at_ && !data.empty()) {
      // This append is where the power goes: part of it may reach the page
      // cache before the crash.
      f_->data.append(data.substr(0, fs_.rng_.uniform(0, data.size() - 1)));
    }
    fs_.mutate();
    f_->data.append(data);
    fs_.stats_.bytes_written += data.size();
  }
  void sync() override {
    fs_.mutate();
    f_->synced = f_->data.size();
    ++fs_.stats_.syncs;
  }
  void close() override {}

 private:
  SimFs& fs_;
  std::shared_ptr<SimFs::File> f_;
};

namespace {

class SimReadable : public ReadableFile {
 public:
  SimReadable(std::shared_ptr<SimFs::File> f, FsStats& stats) : f_(std::move(f)), stats_(stats) {}
  std::uint64_t size() const override { return f_->data.size(); }
  std::string read(std::uint64_t offset, std::size_t n) const override {
    if (offset >= f_->data.size()) return {};
    std::string out = f_->data.substr(offset, n);
    stats_.bytes_read += out.size();
    return out;
  }

 private:
  std::shared_ptr<SimFs::File> f_;
  FsStats& stats_;
};

}  // namespace

void SimFs::mutate() {
  ++mutations_;
  if (crash_at_ != 0 && mutations_ == crash_at_) {
    crash_at_ = 0;
    throw SimCrash{mutations_};
  }
}

std::unique_ptr<WritableFile> SimFs::create(const std::string& path) {
  mutate();
  auto f = std::make_shared<File>();
  files_[path] = f;
  ++stats_.files_created;
  return std::make_unique<SimWritable>(*this, f);
}

std::unique_ptr<ReadableFile> SimFs::open(const std::string& path) {
  auto it = files_.find(path);
  if (it == files_.end()) throw std::runtime_error("open " + path + ": no such file");
  return std::make_unique<SimReadable>(it->second, stats_);
}

bool SimFs::exists(const std::string& path) { return files_.count(path) > 0; }

void SimFs::rename(const std::string& from, const std::string& to) {
  mutate();
  auto it = files_.find(from);
  if (it == files_.end()) throw std::runtime_error("rename " + from + ": no such file");
  auto f = it->second;
  files_.erase(it);
  files_[to] = std::move(f);
}

void SimFs::remove(const std::string& path) {
  mutate();
  files_.erase(path);
}

std::vector<std::string> SimFs::list(const std::string& dir) {
  std::vector<std::string> out;
  const std::string prefix = dir + "/";
  for (auto it = files_.lower_bound(prefix); it != files_.end(); ++it) {
    if (it->first.compare(0, prefix.size(), prefix) != 0) break;
    const std::string name = it->first.substr(prefix.size());
    if (name.find('/') == std::string::npos) out.push_back(name);
  }
  return out;
}

void SimFs::mkdir(const std::string&) {}

void SimFs::crash() {
  crash_at_ = 0;
  for (auto& [path, f] : files_) {
    const std::size_t keep = rng_.uniform(f->synced, f->data.size());
    f->data.resize(keep);
    f->synced = keep;
  }
}

std::string SimFs::contents(const std::string& path) const {
  auto it = files_.find(path);
  return it == files_.end() ? std::string() : it->second->data;
}

std::uint64_t SimFs::durable_size(const std::string& path) const {
  auto it = files_.find(path);
  return it == files_.end() ? 0 : it->second->synced;
}

}  // namespace lsm
