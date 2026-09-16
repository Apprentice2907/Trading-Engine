#define _CRT_SECURE_NO_WARNINGS

#include "hft/market_data.hpp"

#include <cstring>
#include <random>
#include <cassert>
#include <chrono>
#include <emmintrin.h>

namespace hft {

// ============================================================================
// MockMarketDataSource Implementation
// ============================================================================

MockMarketDataSource::MockMarketDataSource(size_t event_count, uint32_t token)
    : event_count_(event_count), token_(token) {}

MockMarketDataSource::~MockMarketDataSource() {
    stop();
}

std::vector<MarketEvent> MockMarketDataSource::generate_events(size_t count, uint32_t token, uint64_t seed) {
    std::vector<MarketEvent> events;
    events.reserve(count);

    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int64_t> price_delta(-50, 50);
    std::uniform_int_distribution<uint64_t> qty_dist(10, 500);

    int64_t base_price = 83000; // 830.00 in paise / cents
    uint64_t exch_ts = 1710000000000000000ULL;
    uint64_t volume = 10000;

    for (size_t i = 0; i < count; ++i) {
        base_price += price_delta(rng);
        if (base_price < 80000) base_price = 80000;
        if (base_price > 86000) base_price = 86000;

        uint64_t qty = qty_dist(rng);
        volume += qty;
        exch_ts += 100000000ULL; // +100ms in ns

        MarketEvent ev{};
        ev.instrument_token = token;
        ev.exchange_type = exchange::NSE;
        ev.subscription_mode = 1;
        ev.sequence_number = i + 1;
        ev.exchange_timestamp = exch_ts;
        ev.receive_timestamp = exch_ts + 500000ULL;
        ev.last_price = base_price;
        ev.last_quantity = qty;
        ev.best_bid_price = base_price - 5;
        ev.best_bid_quantity = qty * 2;
        ev.best_ask_price = base_price + 5;
        ev.best_ask_quantity = qty * 3;
        ev.volume = volume;

        events.push_back(ev);
    }
    return events;
}

bool MockMarketDataSource::start() {
    if (running_.exchange(true)) return false;
    worker_thread_ = std::thread(&MockMarketDataSource::worker_loop, this);
    return true;
}

void MockMarketDataSource::stop() {
    if (running_.exchange(false)) {
        if (worker_thread_.joinable()) {
            worker_thread_.join();
        }
    }
}

void MockMarketDataSource::worker_loop() {
    auto events = generate_events(event_count_, token_);
    for (size_t i = 0; i < events.size() && running_.load(std::memory_order_relaxed); ++i) {
        if (pipeline_) {
            pipeline_->enqueue_event(events[i]);
        }
        if (event_callback_) {
            event_callback_(events[i]);
        }
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
}

// ============================================================================
// ReplayMarketDataSource Implementation
// ============================================================================

ReplayMarketDataSource::ReplayMarketDataSource(std::string mktlog_path)
    : path_(std::move(mktlog_path)) {}

ReplayMarketDataSource::~ReplayMarketDataSource() {
    stop();
}

bool ReplayMarketDataSource::start() {
    if (running_.exchange(true)) return false;
    worker_thread_ = std::thread(&ReplayMarketDataSource::worker_loop, this);
    return true;
}

void ReplayMarketDataSource::stop() {
    if (running_.exchange(false)) {
        if (worker_thread_.joinable()) {
            worker_thread_.join();
        }
    }
}

void ReplayMarketDataSource::worker_loop() {
    MarketEventReplayer replayer;
    if (!replayer.open(path_)) {
        running_.store(false);
        return;
    }

    MarketEvent ev{};
    while (running_.load(std::memory_order_relaxed) && replayer.next(ev)) {
        if (pipeline_) {
            pipeline_->enqueue_event(ev);
        }
        if (event_callback_) {
            event_callback_(ev);
        }
    }
    replayer.close();
}

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
