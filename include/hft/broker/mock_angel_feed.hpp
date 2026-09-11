#pragma once

#include "hft/broker/angel_types.hpp"
#include <cstdint>
#include <cstddef>
#include <vector>

namespace hft::broker {

/**
 * @brief Deterministic mock generator for Angel One SmartStream binary packets.
 *
 * Produces bit-exact LTP (51B), Quote (147B), and SnapQuote (347B) packets
 * for automated offline testing without network or credentials.
 */
class MockAngelFeed {
public:
    static size_t build_ltp_packet(uint8_t* out_buf, size_t buf_size,
                                   const char* token, int64_t ltp_paise,
                                   uint64_t seq, int64_t ts_ms,
                                   uint8_t exchange = AngelConstants::EXCH_NSE_CM);

    static size_t build_quote_packet(uint8_t* out_buf, size_t buf_size,
                                     const char* token, int64_t ltp_paise,
                                     uint64_t last_qty, uint64_t seq,
                                     int64_t ts_ms, uint64_t volume,
                                     int64_t open, int64_t high, int64_t low, int64_t close,
                                     uint8_t exchange = AngelConstants::EXCH_NSE_CM);

    static size_t build_snap_quote_packet(uint8_t* out_buf, size_t buf_size,
                                          const char* token, int64_t ltp_paise,
                                          uint64_t last_qty, int64_t bid_paise,
                                          uint64_t bid_qty, int64_t ask_paise,
                                          uint64_t ask_qty, uint64_t seq,
                                          int64_t ts_ms, uint64_t volume,
                                          uint8_t exchange = AngelConstants::EXCH_NSE_CM);

    /**
     * @brief Generates a batch of N deterministic Quote packets for testing and benchmarks.
     */
    static std::vector<std::vector<uint8_t>> generate_synthetic_stream(size_t count,
                                                                       uint64_t seed = 0x12345678ULL);

private:
    static void write_u64_le(uint8_t* p, uint64_t v) noexcept {
        p[0] = static_cast<uint8_t>(v & 0xFF);
        p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
        p[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
        p[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
        p[4] = static_cast<uint8_t>((v >> 32) & 0xFF);
        p[5] = static_cast<uint8_t>((v >> 40) & 0xFF);
        p[6] = static_cast<uint8_t>((v >> 48) & 0xFF);
        p[7] = static_cast<uint8_t>((v >> 56) & 0xFF);
    }

    static void write_i64_le(uint8_t* p, int64_t v) noexcept {
        write_u64_le(p, static_cast<uint64_t>(v));
    }
};

} // namespace hft::broker
