#pragma once

#include "hft/market_event.hpp"
#include "hft/broker/angel_types.hpp"
#include <cstdint>
#include <cstddef>
#include <string_view>

namespace hft::broker {

/**
 * @brief High-performance zero-allocation binary decoder for Angel One SmartStream packets.
 *
 * Decodes Little-Endian wire packets (Mode 1: LTP, Mode 2: Quote, Mode 3: SnapQuote)
 * into normalized 64-byte MarketEvent structures.
 */
class AngelDecoder {
public:
    AngelDecoder() = default;

    /**
     * @brief Decodes a raw binary packet into a normalized MarketEvent.
     *
     * @param data Pointer to raw packet memory
     * @param length Packet byte length
     * @param out_event Destination MarketEvent
     * @param receive_ts_ns Monotonic local timestamp in nanoseconds
     * @return true if successfully decoded, false if malformed or unrecognized
     */
    static bool decode(const uint8_t* data, size_t length, MarketEvent& out_event,
                       uint64_t receive_ts_ns) noexcept;

    /**
     * @brief Parses an ASCII numeric token (up to 25 bytes) into an integer token ID.
     */
    static uint32_t parse_token(const char* token_bytes, size_t max_len = 25) noexcept;

private:
    static int64_t read_i64_le(const uint8_t* p) noexcept {
        uint64_t v = static_cast<uint64_t>(p[0]) |
                     (static_cast<uint64_t>(p[1]) << 8) |
                     (static_cast<uint64_t>(p[2]) << 16) |
                     (static_cast<uint64_t>(p[3]) << 24) |
                     (static_cast<uint64_t>(p[4]) << 32) |
                     (static_cast<uint64_t>(p[5]) << 40) |
                     (static_cast<uint64_t>(p[6]) << 48) |
                     (static_cast<uint64_t>(p[7]) << 56);
        return static_cast<int64_t>(v);
    }

    static uint64_t read_u64_le(const uint8_t* p) noexcept {
        return static_cast<uint64_t>(p[0]) |
               (static_cast<uint64_t>(p[1]) << 8) |
               (static_cast<uint64_t>(p[2]) << 16) |
               (static_cast<uint64_t>(p[3]) << 24) |
               (static_cast<uint64_t>(p[4]) << 32) |
               (static_cast<uint64_t>(p[5]) << 40) |
               (static_cast<uint64_t>(p[6]) << 48) |
               (static_cast<uint64_t>(p[7]) << 56);
    }
};

} // namespace hft::broker
