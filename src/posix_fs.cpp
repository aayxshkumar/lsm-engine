#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>

#include "lsm/fs.hpp"

namespace lsm {

namespace {

[[noreturn]] void fail(const std::string& what, const std::string& path) {
  throw std::runtime_error(what + " " + path + ": " + std::strerror(errno));
}

class PosixWritable : public WritableFile {
 public:
  PosixWritable(int fd, std::string path, FsStats& stats)
      : fd_(fd), path_(std::move(path)), stats_(stats) {}
  ~PosixWritable() override {
    if (fd_ >= 0) ::close(fd_);
  }
  void append(std::string_view data) override {
    const char* p = data.data();
    std::size_t left = data.size();
    while (left > 0) {
      const ssize_t n = ::write(fd_, p, left);
      if (n < 0) {
        if (errno == EINTR) continue;
        fail("write", path_);
      }
      p += n;
      left -= static_cast<std::size_t>(n);
    }
    stats_.bytes_written += data.size();
  }
  void sync() override {
#ifdef __APPLE__
    // fsync on macOS doesn't flush the drive cache; F_FULLFSYNC does.
    if (::fcntl(fd_, F_FULLFSYNC) != 0 && ::fsync(fd_) != 0) fail("fsync", path_);
#else
    if (::fdatasync(fd_) != 0) fail("fdatasync", path_);
#endif
    ++stats_.syncs;
  }
  void close() override {
    if (fd_ >= 0 && ::close(fd_) != 0) fail("close", path_);
    fd_ = -1;
  }

 private:
  int fd_;
  std::string path_;
  FsStats& stats_;
};

class PosixReadable : public ReadableFile {
 public:
  PosixReadable(int fd, std::string path, std::uint64_t size, FsStats& stats)
      : fd_(fd), path_(std::move(path)), size_(size), stats_(stats) {}
  ~PosixReadable() override { ::close(fd_); }
  std::uint64_t size() const override { return size_; }
  std::string read(std::uint64_t offset, std::size_t n) const override {
    std::string out(n, '\0');
    std::size_t got = 0;
    while (got < n) {
      const ssize_t r = ::pread(fd_, out.data() + got, n - got, static_cast<off_t>(offset + got));
      if (r < 0) {
        if (errno == EINTR) continue;
        fail("pread", path_);
      }
      if (r == 0) break;
      got += static_cast<std::size_t>(r);
    }
    out.resize(got);
    stats_.bytes_read += got;
    return out;
  }

 private:
  int fd_;
  std::string path_;
  std::uint64_t size_;
  FsStats& stats_;
};

}  // namespace

std::unique_ptr<WritableFile> PosixFs::create(const std::string& path) {
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) fail("create", path);
  ++stats_.files_created;
  return std::make_unique<PosixWritable>(fd, path, stats_);
}

std::unique_ptr<ReadableFile> PosixFs::open(const std::string& path) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) fail("open", path);
  struct stat st {};
  if (::fstat(fd, &st) != 0) {
    ::close(fd);
    fail("fstat", path);
  }
  return std::make_unique<PosixReadable>(fd, path, static_cast<std::uint64_t>(st.st_size), stats_);
}

bool PosixFs::exists(const std::string& path) {
  struct stat st {};
  return ::stat(path.c_str(), &st) == 0;
}

void PosixFs::rename(const std::string& from, const std::string& to) {
  if (::rename(from.c_str(), to.c_str()) != 0) fail("rename", from);
}

void PosixFs::remove(const std::string& path) {
  if (::unlink(path.c_str()) != 0 && errno != ENOENT) fail("unlink", path);
}

std::vector<std::string> PosixFs::list(const std::string& dir) {
  std::vector<std::string> out;
  DIR* d = ::opendir(dir.c_str());
  if (!d) fail("opendir", dir);
  while (dirent* e = ::readdir(d)) {
    std::string name = e->d_name;
    if (name != "." && name != "..") out.push_back(std::move(name));
  }
  ::closedir(d);
  return out;
}

void PosixFs::mkdir(const std::string& dir) {
  if (::mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) fail("mkdir", dir);
}

}  // namespace lsm
