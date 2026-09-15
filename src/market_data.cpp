#define _CRT_SECURE_NO_WARNINGS

#include "hft/market_data.hpp"

#include <cstring>
#include <random>
#include <cassert>
#include <chrono>
#include <emmintrin.h>

namespace hft {

// ============================================================================
// AngelDecoder Implementation
// ============================================================================

namespace broker {

uint32_t AngelDecoder::parse_token(const char* token_bytes, size_t max_len) noexcept {
    // Parse at most 9 decimal digits to prevent uint32_t overflow.
    // 10 digits of all-9s = 9,999,999,999 which exceeds UINT32_MAX (4,294,967,295).
    // 9 digits max = 999,999,999 which is always safe.
    static constexpr size_t MAX_SAFE_DIGITS = 9;
    uint32_t token = 0;
    size_t digits = 0;
    for (size_t i = 0; i < max_len && token_bytes[i] != '\0'; ++i) {
        char c = token_bytes[i];
        if (c >= '0' && c <= '9') {
            if (digits >= MAX_SAFE_DIGITS) break; // truncate silently, avoids overflow
            token = token * 10u + static_cast<uint32_t>(c - '0');
            ++digits;
        }
    }
    return token;
}

bool AngelDecoder::decode(const uint8_t* data, size_t length, MarketEvent& out_event,
                          uint64_t receive_ts_ns) noexcept {
    // Require at least the smallest valid packet size before touching any byte.
    if (data == nullptr || length < AngelConstants::PACKET_SIZE_LTP) {
        return false;
    }

    const uint8_t mode     = data[0];
    const uint8_t exchange = data[1];

    // Validate mode and require the actual buffer covers the full packet for that mode.
    if (mode == AngelConstants::MODE_LTP) {
        // length >= PACKET_SIZE_LTP already verified above; no additional check needed.
    } else if (mode == AngelConstants::MODE_QUOTE) {
        if (length < AngelConstants::PACKET_SIZE_QUOTE) return false;
    } else if (mode == AngelConstants::MODE_SNAP_QUOTE) {
        if (length < AngelConstants::PACKET_SIZE_SNAP_QUOTE) return false;
    } else {
        // Unknown or unsupported mode (0, 4=Depth, 255, etc.) — reject safely.
        return false;
    }

    // Validate exchange type is a known value (1–5 and 7, 13).
    // Unknown exchange bytes are accepted for forward-compatibility but flagged
    // by setting exchange_type to 0 so callers can detect it.
    const uint8_t safe_exchange = (exchange == AngelConstants::EXCH_NSE_CM ||
                                   exchange == AngelConstants::EXCH_NSE_FO ||
                                   exchange == AngelConstants::EXCH_BSE_CM ||
                                   exchange == AngelConstants::EXCH_BSE_FO ||
                                   exchange == AngelConstants::EXCH_MCX_FO ||
                                   exchange == AngelConstants::EXCH_NCX_FO ||
                                   exchange == AngelConstants::EXCH_CDE_FO)
                                      ? exchange : 0;

    // All offsets below are safe because we validated length >= required size above:
    //   LTP:        offsets 0..50   (size 51)
    //   Quote:      offsets 0..146  (size 147)
    //   SnapQuote:  offsets 0..346  (size 347); depth reads at 147+10=157, 247+10=257 — both < 347.
    const uint32_t token      = parse_token(reinterpret_cast<const char*>(data + 2), 25);
    const uint64_t seq        = read_u64_le(data + 27);
    const int64_t  ts_ms      = read_i64_le(data + 35);
    const int64_t  ltp_paise  = read_i64_le(data + 43);

    // Clamp timestamp: negative timestamps are invalid; very large values are passed through.
    const uint64_t exch_ts_ns = (ts_ms > 0)
        ? static_cast<uint64_t>(ts_ms) * 1000000ULL
        : 0ULL;

    out_event.instrument_token  = token;
    out_event.exchange_type     = safe_exchange;
    out_event.subscription_mode = mode;
    out_event.pad               = 0;
    out_event.sequence_number   = seq;
    out_event.exchange_timestamp = exch_ts_ns;
    out_event.receive_timestamp  = receive_ts_ns;
    out_event.last_price        = ltp_paise;
    out_event.last_quantity     = 0;
    out_event.best_bid_price    = 0;
    out_event.best_bid_quantity = 0;
    out_event.best_ask_price    = 0;
    out_event.best_ask_quantity = 0;
    out_event.volume            = 0;

    if (mode == AngelConstants::MODE_QUOTE || mode == AngelConstants::MODE_SNAP_QUOTE) {
        // Offsets 51..74 (last_qty @ 51, volume @ 67) — within 147-byte Quote packet.
        out_event.last_quantity = read_u64_le(data + 51);
        out_event.volume        = read_u64_le(data + 67);
    }

    if (mode == AngelConstants::MODE_SNAP_QUOTE) {
        // Depth level 0 bid/ask: within 347-byte SnapQuote packet.
        // bid_qty  @ 149, bid_price @ 157 — within [0, 346].
        // ask_qty  @ 249, ask_price @ 257 — within [0, 346].
        out_event.best_bid_quantity = read_u64_le(data + 147 + 2);
        out_event.best_bid_price    = read_i64_le(data + 147 + 10);
        out_event.best_ask_quantity = read_u64_le(data + 247 + 2);
        out_event.best_ask_price    = read_i64_le(data + 247 + 10);
    }

    return true;
}

// ============================================================================
// MockAngelFeed Implementation
// ============================================================================

size_t MockAngelFeed::build_ltp_packet(uint8_t* out_buf, size_t buf_size,
                                       const char* token, int64_t ltp_paise,
                                       uint64_t seq, int64_t ts_ms,
                                       uint8_t exchange) {
    if (buf_size < AngelConstants::PACKET_SIZE_LTP) return 0;

    std::memset(out_buf, 0, AngelConstants::PACKET_SIZE_LTP);
    out_buf[0] = AngelConstants::MODE_LTP;
    out_buf[1] = exchange;

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
    write_i64_le(out_buf + 59, ltp_paise);
    write_u64_le(out_buf + 67, volume);
    write_i64_le(out_buf + 75, 5000);
    write_i64_le(out_buf + 83, 5000);
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
    write_i64_le(out_buf + 91, ltp_paise - 50);
    write_i64_le(out_buf + 99, ltp_paise + 100);
    write_i64_le(out_buf + 107, ltp_paise - 100);
    write_i64_le(out_buf + 115, ltp_paise - 10);

    out_buf[147] = 0;
    out_buf[148] = 0;
    write_u64_le(out_buf + 147 + 2, bid_qty);
    write_i64_le(out_buf + 147 + 10, bid_paise);

    out_buf[247] = 1;
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

    std::vector<uint8_t> buf(AngelConstants::PACKET_SIZE_SNAP_QUOTE);

    for (size_t i = 0; i < count; ++i) {
        base_price += price_delta(rng);
        if (base_price < 80000) base_price = 80000;
        if (base_price > 86000) base_price = 86000;

        uint64_t qty = qty_dist(rng);
        volume += qty;
        ts_ms += 100;

        int64_t bid_price = base_price - 5;
        int64_t ask_price = base_price + 5;
        uint64_t bid_qty = qty * 2;
        uint64_t ask_qty = qty * 3;

        size_t len = build_snap_quote_packet(buf.data(), buf.size(), token,
                                             base_price, qty,
                                             bid_price, bid_qty,
                                             ask_price, ask_qty,
                                             i + 1, ts_ms, volume);
        std::vector<uint8_t> pkt(buf.data(), buf.data() + len);
        stream.push_back(std::move(pkt));
    }

    return stream;
}

} // namespace broker

// ============================================================================
// MarketEventRecorder Implementation
// ============================================================================

MarketEventRecorder::MarketEventRecorder(size_t buffer_size)
    : buffer_(buffer_size) {
    assert(buffer_size >= sizeof(MarketEvent) && "Buffer must hold at least one MarketEvent");
}

MarketEventRecorder::~MarketEventRecorder() {
    close();
}

MarketEventRecorder::MarketEventRecorder(MarketEventRecorder&& other) noexcept
    : file_(other.file_),
      file_path_(std::move(other.file_path_)),
      buffer_(std::move(other.buffer_)),
      buffer_pos_(other.buffer_pos_),
      events_written_(other.events_written_),
      data_crc32_(other.data_crc32_) {
    other.file_ = nullptr;
    other.buffer_pos_ = 0;
    other.events_written_ = 0;
    other.data_crc32_ = 0;
}

MarketEventRecorder& MarketEventRecorder::operator=(MarketEventRecorder&& other) noexcept {
    if (this != &other) {
        close();
        file_ = other.file_;
        file_path_ = std::move(other.file_path_);
        buffer_ = std::move(other.buffer_);
        buffer_pos_ = other.buffer_pos_;
        events_written_ = other.events_written_;
        data_crc32_ = other.data_crc32_;

        other.file_ = nullptr;
        other.buffer_pos_ = 0;
        other.events_written_ = 0;
        other.data_crc32_ = 0;
    }
    return *this;
}

bool MarketEventRecorder::open(const std::string& path) {
    close();

    file_path_ = path;
    file_ = std::fopen(path.c_str(), "wb");
    if (!file_) {
        return false;
    }

    events_written_ = 0;
    data_crc32_ = 0;
    buffer_pos_ = 0;

    // Reserve 64 bytes for header
    MarketFileHeader placeholder{};
    size_t written = std::fwrite(&placeholder, 1, sizeof(placeholder), file_);
    if (written != sizeof(placeholder)) {
        close();
        return false;
    }

    return true;
}

bool MarketEventRecorder::write(const MarketEvent& ev) noexcept {
    if (!file_) return false;

    if (buffer_pos_ + sizeof(MarketEvent) > buffer_.size()) {
        flush();
    }

    std::memcpy(buffer_.data() + buffer_pos_, &ev, sizeof(MarketEvent));
    buffer_pos_ += sizeof(MarketEvent);
    ++events_written_;
    return true;
}

void MarketEventRecorder::flush() {
    if (!file_ || buffer_pos_ == 0) return;

    data_crc32_ = crc32(data_crc32_, buffer_.data(), buffer_pos_);
    std::fwrite(buffer_.data(), 1, buffer_pos_, file_);
    buffer_pos_ = 0;
}

void MarketEventRecorder::close() {
    if (!file_) return;

    flush();

    // Finalize header
    MarketFileHeader hdr{};
    hdr.magic = MKT_LOG_MAGIC;
    hdr.version = MKT_LOG_VERSION;
    hdr.record_size = MKT_LOG_RECORD_SIZE;
    hdr.header_size = sizeof(MarketFileHeader);
    hdr.event_count = events_written_;
    hdr.data_crc32 = data_crc32_;
    hdr.header_crc32 = compute_market_header_crc32(hdr);

    std::fseek(file_, 0, SEEK_SET);
    std::fwrite(&hdr, 1, sizeof(hdr), file_);
    std::fclose(file_);
    file_ = nullptr;
}

// ============================================================================
// MarketEventReplayer Implementation
// ============================================================================

MarketEventReplayer::MarketEventReplayer(size_t buffer_size)
    : buffer_(buffer_size) {
    assert(buffer_size >= sizeof(MarketEvent));
}

MarketEventReplayer::~MarketEventReplayer() {
    close();
}

MarketEventReplayer::MarketEventReplayer(MarketEventReplayer&& other) noexcept
    : file_(other.file_),
      file_path_(std::move(other.file_path_)),
      header_(other.header_),
      buffer_(std::move(other.buffer_)),
      buffer_pos_(other.buffer_pos_),
      buffer_valid_(other.buffer_valid_),
      events_read_(other.events_read_),
      eof_reached_(other.eof_reached_) {
    other.file_ = nullptr;
    other.buffer_pos_ = 0;
    other.buffer_valid_ = 0;
    other.events_read_ = 0;
    other.eof_reached_ = false;
}

MarketEventReplayer& MarketEventReplayer::operator=(MarketEventReplayer&& other) noexcept {
    if (this != &other) {
        close();
        file_ = other.file_;
        file_path_ = std::move(other.file_path_);
        header_ = other.header_;
        buffer_ = std::move(other.buffer_);
        buffer_pos_ = other.buffer_pos_;
        buffer_valid_ = other.buffer_valid_;
        events_read_ = other.events_read_;
        eof_reached_ = other.eof_reached_;

        other.file_ = nullptr;
        other.buffer_pos_ = 0;
        other.buffer_valid_ = 0;
        other.events_read_ = 0;
        other.eof_reached_ = false;
    }
    return *this;
}

bool MarketEventReplayer::open(const std::string& path, std::string* error_out) {
    close();

    auto fail = [&](std::string msg) {
        if (error_out) *error_out = std::move(msg);
        close();
        return false;
    };

    file_ = std::fopen(path.c_str(), "rb");
    if (!file_) return fail("Cannot open file: " + path);

    file_path_ = path;

    size_t read_bytes = std::fread(&header_, 1, sizeof(header_), file_);
    if (read_bytes != sizeof(header_)) return fail("Truncated header");

    if (header_.magic != MKT_LOG_MAGIC) return fail("Invalid magic identifier");
    if (header_.version != MKT_LOG_VERSION) return fail("Unsupported format version");
    if (header_.record_size != MKT_LOG_RECORD_SIZE) return fail("Unexpected record size");

    const uint32_t expected_hdr_crc = compute_market_header_crc32(header_);
    if (header_.header_crc32 != expected_hdr_crc) return fail("Header CRC32 mismatch");

    events_read_ = 0;
    buffer_pos_ = 0;
    buffer_valid_ = 0;
    eof_reached_ = false;

    return true;
}

bool MarketEventReplayer::next(MarketEvent& ev) noexcept {
    if (!file_ || eof_reached_ || events_read_ >= header_.event_count) {
        return false;
    }

    if (buffer_pos_ + sizeof(MarketEvent) > buffer_valid_) {
        // Refill buffer
        buffer_valid_ = std::fread(buffer_.data(), 1, buffer_.size(), file_);
        buffer_pos_ = 0;
        if (buffer_valid_ < sizeof(MarketEvent)) {
            eof_reached_ = true;
            return false;
        }
    }

    std::memcpy(&ev, buffer_.data() + buffer_pos_, sizeof(MarketEvent));
    buffer_pos_ += sizeof(MarketEvent);
    ++events_read_;
    return true;
}

bool MarketEventReplayer::validate_full_checksum(std::string* error_out) {
    if (!file_) {
        if (error_out) *error_out = "No file open";
        return false;
    }

    long current_pos = std::ftell(file_);
    std::fseek(file_, sizeof(MarketFileHeader), SEEK_SET);

    uint32_t computed_crc = 0;
    std::vector<uint8_t> check_buf(65536);

    while (true) {
        size_t n = std::fread(check_buf.data(), 1, check_buf.size(), file_);
        if (n == 0) break;
        computed_crc = crc32(computed_crc, check_buf.data(), n);
    }

    std::fseek(file_, current_pos, SEEK_SET);

    if (computed_crc != header_.data_crc32) {
        if (error_out) *error_out = "Payload CRC32 mismatch";
        return false;
    }

    return true;
}

void MarketEventReplayer::close() {
    if (file_) {
        std::fclose(file_);
        file_ = nullptr;
    }
}

// ============================================================================
// MarketDataPipeline Implementation
// ============================================================================

MarketDataPipeline::MarketDataPipeline(size_t capacity)
    : queue_(std::make_unique<SpscQueue<MarketEvent, DEFAULT_QUEUE_CAPACITY, true>>()) {
    (void)capacity;
}

MarketDataPipeline::~MarketDataPipeline() {
    if (running_.load(std::memory_order_relaxed)) {
        stop_and_join();
    }
}

void MarketDataPipeline::start() {
    if (running_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    consumer_thread_ = std::thread(&MarketDataPipeline::consumer_loop, this);
}

void MarketDataPipeline::stop_and_join() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    if (consumer_thread_.joinable()) {
        consumer_thread_.join();
    }
}

bool MarketDataPipeline::enqueue_event(const MarketEvent& ev) noexcept {
    if (queue_->try_push(ev)) {
        total_enqueued_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    total_dropped_.fetch_add(1, std::memory_order_relaxed);
    return false;
}

void MarketDataPipeline::enqueue_event_wait(const MarketEvent& ev) noexcept {
    while (!queue_->try_push(ev)) {
        _mm_pause();
    }
    total_enqueued_.fetch_add(1, std::memory_order_relaxed);
}

std::optional<MarketEvent> MarketDataPipeline::latest_event() const noexcept {
    if (!has_latest_.load(std::memory_order_acquire)) {
        return std::nullopt;
    }
    return latest_event_;
}

void MarketDataPipeline::consumer_loop() {
    MarketEvent ev{};
    while (running_.load(std::memory_order_relaxed)) {
        if (queue_->try_pop(ev)) {
            total_consumed_.fetch_add(1, std::memory_order_relaxed);
            latest_event_ = ev;
            has_latest_.store(true, std::memory_order_release);
            if (event_listener_) {
                event_listener_(ev);
            }
        } else {
            _mm_pause();
        }
    }

    while (queue_->try_pop(ev)) {
        total_consumed_.fetch_add(1, std::memory_order_relaxed);
        latest_event_ = ev;
        has_latest_.store(true, std::memory_order_release);
        if (event_listener_) {
            event_listener_(ev);
        }
    }
}

} // namespace hft
