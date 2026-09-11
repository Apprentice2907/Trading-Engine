#include "hft/broker/mock_angel_feed.hpp"
#include <cstring>
#include <random>

namespace hft::broker {

size_t MockAngelFeed::build_ltp_packet(uint8_t* out_buf, size_t buf_size,
                                       const char* token, int64_t ltp_paise,
                                       uint64_t seq, int64_t ts_ms,
                                       uint8_t exchange) {
    if (buf_size < AngelConstants::PACKET_SIZE_LTP) return 0;

    std::memset(out_buf, 0, AngelConstants::PACKET_SIZE_LTP);
    out_buf[0] = AngelConstants::MODE_LTP;
    out_buf[1] = exchange;

    // Copy token up to 24 chars + null terminator
    size_t tok_len = std::strlen(token);
    if (tok_len > 24) tok_len = 24;
    std::memcpy(out_buf + 2, token, tok_len);

    write_u64_le(out_buf + 27, seq);
    write_i64_le(out_buf + 35, ts_ms);
    write_i64_le(out_buf + 43, ltp_paise);

    return AngelConstants::PACKET_SIZE_LTP;
}

size_t MockAngelFeed::build_quote_packet(uint8_t* out_buf, size_t buf_size,
                                         const char* token, int64_t ltp_paise,
                                         uint64_t last_qty, uint64_t seq,
                                         int64_t ts_ms, uint64_t volume,
                                         int64_t open, int64_t high, int64_t low, int64_t close,
                                         uint8_t exchange) {
    if (buf_size < AngelConstants::PACKET_SIZE_QUOTE) return 0;

    std::memset(out_buf, 0, AngelConstants::PACKET_SIZE_QUOTE);
    out_buf[0] = AngelConstants::MODE_QUOTE;
    out_buf[1] = exchange;

    size_t tok_len = std::strlen(token);
    if (tok_len > 24) tok_len = 24;
    std::memcpy(out_buf + 2, token, tok_len);

    write_u64_le(out_buf + 27, seq);
    write_i64_le(out_buf + 35, ts_ms);
    write_i64_le(out_buf + 43, ltp_paise);

    write_u64_le(out_buf + 51, last_qty);
    write_i64_le(out_buf + 59, ltp_paise); // avg price
    write_u64_le(out_buf + 67, volume);
    write_i64_le(out_buf + 75, 5000);     // buy qty
    write_i64_le(out_buf + 83, 5000);     // sell qty
    write_i64_le(out_buf + 91, open);
    write_i64_le(out_buf + 99, high);
    write_i64_le(out_buf + 107, low);
    write_i64_le(out_buf + 115, close);

    return AngelConstants::PACKET_SIZE_QUOTE;
}

size_t MockAngelFeed::build_snap_quote_packet(uint8_t* out_buf, size_t buf_size,
                                              const char* token, int64_t ltp_paise,
                                              uint64_t last_qty, int64_t bid_paise,
                                              uint64_t bid_qty, int64_t ask_paise,
                                              uint64_t ask_qty, uint64_t seq,
                                              int64_t ts_ms, uint64_t volume,
                                              uint8_t exchange) {
    if (buf_size < AngelConstants::PACKET_SIZE_SNAP_QUOTE) return 0;

    std::memset(out_buf, 0, AngelConstants::PACKET_SIZE_SNAP_QUOTE);
    out_buf[0] = AngelConstants::MODE_SNAP_QUOTE;
    out_buf[1] = exchange;

    size_t tok_len = std::strlen(token);
    if (tok_len > 24) tok_len = 24;
    std::memcpy(out_buf + 2, token, tok_len);

    write_u64_le(out_buf + 27, seq);
    write_i64_le(out_buf + 35, ts_ms);
    write_i64_le(out_buf + 43, ltp_paise);

    write_u64_le(out_buf + 51, last_qty);
    write_i64_le(out_buf + 59, ltp_paise);
    write_u64_le(out_buf + 67, volume);
    write_i64_le(out_buf + 75, bid_qty);
    write_i64_le(out_buf + 83, ask_qty);
    write_i64_le(out_buf + 91, ltp_paise - 50);  // open
    write_i64_le(out_buf + 99, ltp_paise + 100); // high
    write_i64_le(out_buf + 107, ltp_paise - 100);// low
    write_i64_le(out_buf + 115, ltp_paise - 10); // close

    // Depth: Level 1 Bid (offset 147)
    out_buf[147] = 0; // Flag: Buy
    out_buf[148] = 0;
    write_u64_le(out_buf + 147 + 2, bid_qty);
    write_i64_le(out_buf + 147 + 10, bid_paise);

    // Depth: Level 1 Ask (offset 147 + 5 * 20 = 247)
    out_buf[247] = 1; // Flag: Sell
    out_buf[248] = 0;
    write_u64_le(out_buf + 247 + 2, ask_qty);
    write_i64_le(out_buf + 247 + 10, ask_paise);

    return AngelConstants::PACKET_SIZE_SNAP_QUOTE;
}

std::vector<std::vector<uint8_t>> MockAngelFeed::generate_synthetic_stream(size_t count, uint64_t seed) {
    std::vector<std::vector<uint8_t>> stream;
    stream.reserve(count);

    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int64_t> price_delta(-25, 25);
    std::uniform_int_distribution<uint64_t> qty_dist(10, 500);

    const char* token = "3045"; // SBIN
    int64_t base_price = 83000; // 830.00 in paise
    int64_t ts_ms = 1710000000000LL;
    uint64_t volume = 10000;

    for (size_t i = 0; i < count; ++i) {
        int64_t ltp = base_price + price_delta(rng);
        if (ltp <= 100) ltp = 100;
        uint64_t qty = qty_dist(rng);
        int64_t bid = ltp - 5;
        int64_t ask = ltp + 5;
        volume += qty;
        ts_ms += 100; // +100ms per tick

        std::vector<uint8_t> pkt(AngelConstants::PACKET_SIZE_SNAP_QUOTE);
        build_snap_quote_packet(pkt.data(), pkt.size(), token, ltp, qty,
                                bid, qty * 2, ask, qty * 3, i + 1, ts_ms, volume);
        stream.push_back(std::move(pkt));
    }

    return stream;
}

} // namespace hft::broker
