#pragma once

#include <cstdint>
#include <string_view>

namespace lsm {

// CRC-32C (Castagnoli), table-driven. Not hardware accelerated yet.
std::uint32_t crc32c(std::string_view data, std::uint32_t crc = 0);

}  // namespace lsm
