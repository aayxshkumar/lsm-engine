#include "lsm/wal.hpp"

#include "lsm/coding.hpp"
#include "lsm/crc32c.hpp"
#include "lsm/format.hpp"

namespace lsm {

std::string WriteBatch::encode(SequenceNumber first_seq) const {
  std::string out;
  put_fixed64(out, first_seq);
  put_fixed32(out, static_cast<std::uint32_t>(ops_.size()));
  for (const Op& op : ops_) {
    out.push_back(static_cast<char>(op.type));
    put_bytes(out, op.key);
    put_bytes(out, op.value);
  }
  return out;
}

WriteBatch WriteBatch::decode(std::string_view payload, SequenceNumber& first_seq) {
  Decoder d(payload);
  first_seq = d.fixed64();
  const std::uint32_t n = d.fixed32();
  WriteBatch b;
  for (std::uint32_t i = 0; i < n; ++i) {
    const auto t = d.u8();
    if (t > 1) throw Corruption("bad op type in write batch");
    std::string key(d.bytes());
    std::string value(d.bytes());
    if (t == 1) b.put(std::move(key), std::move(value));
    else b.del(std::move(key));
  }
  if (!d.done()) throw Corruption("trailing bytes in write batch");
  return b;
}

void LogWriter::add(std::string_view payload) {
  std::string len;
  put_fixed32(len, static_cast<std::uint32_t>(payload.size()));
  std::string rec;
  rec.reserve(8 + payload.size());
  put_fixed32(rec, crc32c(payload, crc32c(len)));
  rec += len;
  rec.append(payload);
  // One append per record, so a crash tears at most this record.
  file_->append(rec);
}

LogReadResult read_log(const ReadableFile& file) {
  LogReadResult r;
  const std::string data = file.read(0, file.size());
  std::size_t pos = 0;
  while (pos < data.size()) {
    if (data.size() - pos < 8) {
      r.torn_tail = true;
      break;
    }
    const std::uint32_t crc = decode_fixed32(data.data() + pos);
    const std::uint32_t len = decode_fixed32(data.data() + pos + 4);
    if (data.size() - pos - 8 < len) {
      r.torn_tail = true;
      break;
    }
    const std::string_view payload(data.data() + pos + 8, len);
    const std::string_view len_bytes(data.data() + pos + 4, 4);
    if (crc32c(payload, crc32c(len_bytes)) != crc) {
      r.torn_tail = true;
      break;
    }
    r.records.emplace_back(payload);
    pos += 8 + len;
  }
  r.valid_bytes = pos;
  return r;
}

}  // namespace lsm
