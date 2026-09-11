#pragma once

#include "hft/event.hpp"

#include <cstdint>
#include <cstddef>
#include <array>
#include <string_view>

namespace hft {

/**
 * @brief Binary file magic identifier for .hftlog: 'HFTL' in little-endian.
 */
inline constexpr uint32_t HFT_LOG_MAGIC = 0x4C544648; // 'H', 'F', 'T', 'L'

/**
 * @brief Current .hftlog binary format specification version.
 */
inline constexpr uint16_t HFT_LOG_VERSION = 1;

/**
 * @brief Expected binary record size matching the 32-byte OrderEvent.
 */
inline constexpr uint16_t HFT_LOG_RECORD_SIZE = 32;

/**
 * @brief Fixed 64-byte file header for .hftlog files.
 *
 * Sized to exactly 64 bytes to guarantee hardware cache-line alignment
 * for the start of the event stream payload at offset 64.
 */
#pragma pack(push, 1)
struct FileHeader {
    uint32_t magic{HFT_LOG_MAGIC};        // 0x4C544648 ("HFTL")
    uint16_t version{HFT_LOG_VERSION};    // Format version (1)
    uint16_t record_size{HFT_LOG_RECORD_SIZE}; // Size of each event record (32)
    uint32_t header_size{64};            // Total header size in bytes
    uint32_t header_crc32{0};            // CRC32 of first 12 bytes of header
    uint64_t event_count{0};             // Total number of OrderEvents recorded
    uint32_t data_crc32{0};              // CRC32 of entire event payload
    uint32_t flags{0};                   // Optional flags (reserved)
    uint8_t  reserved[32]{0};            // Future expansion padding to exactly 64 bytes
};
#pragma pack(pop)

static_assert(sizeof(FileHeader) == 64, "FileHeader must be exactly 64 bytes");

// ============================================================================
// IEEE 802.3 CRC32 Implementation (Zero External Dependencies)
// ============================================================================

namespace detail {

constexpr auto generate_crc32_table() {
    std::array<uint32_t, 256> table{};
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t crc = i;
        for (int j = 0; j < 8; ++j) {
            crc = (crc & 1) ? (0xEDB88320U ^ (crc >> 1)) : (crc >> 1);
        }
        table[i] = crc;
    }
    return table;
}

inline constexpr auto CRC32_TABLE = generate_crc32_table();

} // namespace detail

/**
 * @brief Computes or incrementally updates an IEEE 802.3 CRC32 checksum.
 *
 * @param previous_crc Previous CRC32 value (pass 0 for new checksum)
 * @param data Pointer to input buffer
 * @param length Number of bytes to process
 * @return Updated CRC32 checksum
 */
inline uint32_t crc32(uint32_t previous_crc, const void* data, size_t length) noexcept {
    const auto* p = static_cast<const uint8_t*>(data);
    uint32_t c = ~previous_crc;
    for (size_t i = 0; i < length; ++i) {
        c = detail::CRC32_TABLE[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    }
    return ~c;
}

/**
 * @brief Computes the checksum of the first 12 bytes of a FileHeader.
 */
inline uint32_t compute_header_crc32(const FileHeader& hdr) noexcept {
    // Checksum magic (4B) + version (2B) + record_size (2B) + header_size (4B) = 12 bytes
    return crc32(0, &hdr, 12);
}

} // namespace hft
