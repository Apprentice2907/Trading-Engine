#include "hft/broker/angel_decoder.hpp"
#include <cstring>

namespace hft::broker {

uint32_t AngelDecoder::parse_token(const char* token_bytes, size_t max_len) noexcept {
    uint32_t token = 0;
    for (size_t i = 0; i < max_len && token_bytes[i] != '\0'; ++i) {
        char c = token_bytes[i];
        if (c >= '0' && c <= '9') {
            token = token * 10 + static_cast<uint32_t>(c - '0');
        }
    }
    return token;
}

bool AngelDecoder::decode(const uint8_t* data, size_t length, MarketEvent& out_event,
                          uint64_t receive_ts_ns) noexcept {
    if (data == nullptr || length < AngelConstants::PACKET_SIZE_LTP) {
        return false;
    }

    const uint8_t mode = data[0];
    const uint8_t exchange = data[1];

    // Validate expected length for requested subscription mode
    if (mode == AngelConstants::MODE_LTP) {
        if (length < AngelConstants::PACKET_SIZE_LTP) return false;
    } else if (mode == AngelConstants::MODE_QUOTE) {
        if (length < AngelConstants::PACKET_SIZE_QUOTE) return false;
    } else if (mode == AngelConstants::MODE_SNAP_QUOTE) {
        if (length < AngelConstants::PACKET_SIZE_SNAP_QUOTE) return false;
    } else {
        // Unknown or unsupported mode
        return false;
    }

    // 1. Common LTP Fields (Offsets 0 - 50)
    const uint32_t token = parse_token(reinterpret_cast<const char*>(data + 2), 25);
    const uint64_t seq = read_u64_le(data + 27);
    const int64_t ts_ms = read_i64_le(data + 35);
    const int64_t ltp_paise = read_i64_le(data + 43);

    const uint64_t exch_ts_ns = (ts_ms > 0)
        ? static_cast<uint64_t>(ts_ms) * 1000000ULL
        : 0ULL;

    out_event.instrument_token = token;
    out_event.exchange_type = exchange;
    out_event.subscription_mode = mode;
    out_event.pad = 0;
    out_event.sequence_number = seq;
    out_event.exchange_timestamp = exch_ts_ns;
    out_event.receive_timestamp = receive_ts_ns;
    out_event.last_price = ltp_paise;
    out_event.last_quantity = 0;
    out_event.best_bid_price = 0;
    out_event.best_bid_quantity = 0;
    out_event.best_ask_price = 0;
    out_event.best_ask_quantity = 0;
    out_event.volume = 0;

    // 2. Quote Fields (Offsets 51 - 146)
    if (mode == AngelConstants::MODE_QUOTE || mode == AngelConstants::MODE_SNAP_QUOTE) {
        out_event.last_quantity = read_u64_le(data + 51);
        out_event.volume        = read_u64_le(data + 67);
    }

    // 3. SnapQuote Market Depth (Offsets 147 - 346)
    if (mode == AngelConstants::MODE_SNAP_QUOTE) {
        // Best Bid: First depth level (Offset 147)
        // 20 bytes: Flag (2B), Qty (8B), Price (8B in paise), Orders (2B)
        out_event.best_bid_quantity = read_u64_le(data + 147 + 2);
        out_event.best_bid_price    = read_i64_le(data + 147 + 10);

        // Best Ask: Sixth depth level (Offset 147 + 5 * 20 = 247)
        out_event.best_ask_quantity = read_u64_le(data + 247 + 2);
        out_event.best_ask_price    = read_i64_le(data + 247 + 10);
    }

    return true;
}

} // namespace hft::broker
