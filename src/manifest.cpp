#include "lsm/manifest.hpp"

#include "lsm/coding.hpp"
#include "lsm/crc32c.hpp"

namespace lsm {

namespace {
constexpr std::uint64_t kMagic = 0x6c736d2d6d616e31ULL;  // "lsm-man1"
}

std::string Manifest::encode() const {
  std::string out;
  put_fixed64(out, kMagic);
  put_fixed64(out, next_file);
  put_fixed64(out, last_sequence);
  put_fixed64(out, log_number);
  put_fixed32(out, static_cast<std::uint32_t>(files.size()));
  for (const auto& f : files) {
    out.push_back(static_cast<char>(f.level));
    put_fixed64(out, f.number);
    put_fixed64(out, f.size);
    put_bytes(out, f.smallest);
    put_bytes(out, f.largest);
  }
  put_fixed32(out, crc32c(out));
  return out;
}

Manifest Manifest::decode(std::string_view data) {
  if (data.size() < 4) throw Corruption("manifest too short");
  const auto body = data.substr(0, data.size() - 4);
  if (crc32c(body) != decode_fixed32(data.data() + data.size() - 4))
    throw Corruption("manifest checksum mismatch");
  Decoder d(body);
  if (d.fixed64() != kMagic) throw Corruption("bad manifest magic");
  Manifest m;
  m.next_file = d.fixed64();
  m.last_sequence = d.fixed64();
  m.log_number = d.fixed64();
  const auto n = d.fixed32();
  for (std::uint32_t i = 0; i < n; ++i) {
    FileMeta f;
    f.level = d.u8();
    f.number = d.fixed64();
    f.size = d.fixed64();
    f.smallest = std::string(d.bytes());
    f.largest = std::string(d.bytes());
    m.files.push_back(std::move(f));
  }
  if (!d.done()) throw Corruption("trailing bytes in manifest");
  return m;
}

void write_manifest(FileSystem& fs, const std::string& dir, const Manifest& m) {
  const std::string tmp = dir + "/MANIFEST.tmp";
  auto f = fs.create(tmp);
  f->append(m.encode());
  f->sync();
  f->close();
  fs.rename(tmp, dir + "/MANIFEST");
}

bool read_manifest(FileSystem& fs, const std::string& dir, Manifest& out) {
  const std::string path = dir + "/MANIFEST";
  if (!fs.exists(path)) return false;
  auto f = fs.open(path);
  out = Manifest::decode(f->read(0, f->size()));
  return true;
}

}  // namespace lsm
