#pragma once

#include "hft/market_event.hpp"
#include "hft/binary_format.hpp"

#include <cstdint>
#include <cstddef>

namespace hft {

/**
 * @brief Binary file magic identifier for .mktlog: 'MKTL' in little-endian.
 */
inline constexpr uint32_t MKT_LOG_MAGIC = 0x4C544B4D; // 'M', 'K', 'T', 'L'

/**
 * @brief Current .mktlog binary format specification version.
 */
inline constexpr uint16_t MKT_LOG_VERSION = 1;

/**
 * @brief Expected binary record size matching the 128-byte MarketEvent.
 */
inline constexpr uint16_t MKT_LOG_RECORD_SIZE = 128;

#pragma pack(push, 1)
struct MarketFileHeader {
    uint32_t magic{MKT_LOG_MAGIC};             // 0x4C544B4D ("MKTL")
    uint16_t version{MKT_LOG_VERSION};         // Format version (1)
    uint16_t record_size{MKT_LOG_RECORD_SIZE}; // Size of each record (64)
    uint32_t header_size{64};                  // Header size in bytes (64)
    uint32_t header_crc32{0};                  // CRC32 of first 12 bytes
    uint64_t event_count{0};                   // Total MarketEvents recorded
    uint32_t data_crc32{0};                    // CRC32 of entire MarketEvent payload
    uint32_t flags{0};                         // Reserved flags
    uint8_t  reserved[32]{0};                  // Future expansion padding to 64 bytes
};
#pragma pack(pop)

static_assert(sizeof(MarketFileHeader) == 64, "MarketFileHeader must be exactly 64 bytes");

inline uint32_t compute_market_header_crc32(const MarketFileHeader& hdr) noexcept {
    return crc32(0, &hdr, 12);
}

} // namespace hft
