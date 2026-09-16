#pragma once

#include "hft/types.hpp"
#include "hft/spsc_queue.hpp"

#include <cstdint>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>
#include <array>
#include <functional>
#include <atomic>
#include <thread>
#include <memory>
#include <optional>
#include <cstdio>
#include <type_traits>

namespace hft {

// ============================================================================
// 1. Normalized Market Event (128 Bytes, Cache-Aligned)
// ============================================================================

struct alignas(64) MarketEvent {
    uint32_t instrument_token{0};   // Numeric instrument token / security ID (4B)
    uint8_t  exchange_type{0};      // Exchange identifier: 1=NSE_CM, 2=NSE_FO, etc. (1B)
    uint8_t  subscription_mode{0};  // Mode received: 1=LTP, 2=Quote, 3=SnapQuote (1B)
    uint16_t pad{0};                // Explicit alignment padding (2B)
    uint64_t sequence_number{0};    // Packet sequence number from exchange (8B)
    uint64_t exchange_timestamp{0}; // Provider timestamp in epoch nanoseconds (8B)
    uint64_t receive_timestamp{0};  // Monotonic steady_clock in nanoseconds (8B)
    int64_t  last_price{0};         // Last traded price in discrete ticks / paise (8B)
    uint64_t last_quantity{0};      // Last traded quantity (8B)
    int64_t  best_bid_price{0};     // Best bid price in discrete ticks / paise (8B)
    uint64_t best_bid_quantity{0};  // Quantity resting at best bid (8B)
    int64_t  best_ask_price{0};     // Best ask price in discrete ticks / paise (8B)
    uint64_t best_ask_quantity{0};  // Quantity resting at best ask (8B)
    uint64_t volume{0};             // Cumulative daily volume traded (8B)
    uint8_t  reserved[40]{0};       // Reserved padding to exactly 128 bytes (40B)

    constexpr bool operator==(const MarketEvent& other) const noexcept {
        return instrument_token == other.instrument_token &&
               exchange_type == other.exchange_type &&
               subscription_mode == other.subscription_mode &&
               sequence_number == other.sequence_number &&
               exchange_timestamp == other.exchange_timestamp &&
               receive_timestamp == other.receive_timestamp &&
               last_price == other.last_price &&
               last_quantity == other.last_quantity &&
               best_bid_price == other.best_bid_price &&
               best_bid_quantity == other.best_bid_quantity &&
               best_ask_price == other.best_ask_price &&
               best_ask_quantity == other.best_ask_quantity &&
               volume == other.volume;
    }
};

static_assert(sizeof(MarketEvent) == 128, "MarketEvent must be exactly 128 bytes");
static_assert(alignof(MarketEvent) == 64, "MarketEvent must be 64-byte cache-line aligned");
static_assert(std::is_trivially_copyable_v<MarketEvent>, "MarketEvent must be trivially copyable");
static_assert(std::is_standard_layout_v<MarketEvent>, "MarketEvent must have standard layout");

// ============================================================================
// 2. IEEE 802.3 CRC32 Implementation
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

inline uint32_t crc32(uint32_t previous_crc, const void* data, size_t length) noexcept {
    const auto* p = static_cast<const uint8_t*>(data);
    uint32_t c = ~previous_crc;
    for (size_t i = 0; i < length; ++i) {
        c = detail::CRC32_TABLE[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    }
    return ~c;
}

// ============================================================================
// 3. Binary File Format (.mktlog)
// ============================================================================

inline constexpr uint32_t MKT_LOG_MAGIC = 0x4C544B4D; // 'M', 'K', 'T', 'L'
inline constexpr uint16_t MKT_LOG_VERSION = 1;
inline constexpr uint16_t MKT_LOG_RECORD_SIZE = 128;

#pragma pack(push, 1)
struct MarketFileHeader {
    uint32_t magic{MKT_LOG_MAGIC};
    uint16_t version{MKT_LOG_VERSION};
    uint16_t record_size{MKT_LOG_RECORD_SIZE};
    uint32_t header_size{64};
    uint32_t header_crc32{0};
    uint64_t event_count{0};
    uint32_t data_crc32{0};
    uint32_t flags{0};
    uint8_t  reserved[32]{0};
};
#pragma pack(pop)

static_assert(sizeof(MarketFileHeader) == 64, "MarketFileHeader must be exactly 64 bytes");

inline uint32_t compute_market_header_crc32(const MarketFileHeader& hdr) noexcept {
    return crc32(0, &hdr, 12);
}

// Forward declaration
class MarketDataPipeline;

namespace exchange {
inline constexpr uint8_t UNKNOWN = 0;
inline constexpr uint8_t NSE     = 1;
inline constexpr uint8_t NASDAQ  = 2;
inline constexpr uint8_t BSE     = 3;
} // namespace exchange

// ============================================================================
// 4. Market Data Source Abstraction & Providers
// ============================================================================

class IMarketDataSource {
public:
    using EventCallback = std::function<void(const MarketEvent&)>;
    using MarketEventCallback = EventCallback;

    virtual ~IMarketDataSource() = default;
    virtual void set_event_callback(EventCallback cb) = 0;
    void set_callback(EventCallback cb) { set_event_callback(std::move(cb)); }
    virtual bool start() = 0;
    virtual void stop() = 0;
    [[nodiscard]] virtual bool running() const noexcept = 0;
    [[nodiscard]] bool is_running() const noexcept { return running(); }
};

// --- Mock Market Data Source (Deterministic Synthetic Feeds) ---
class MockMarketDataSource : public IMarketDataSource {
public:
    explicit MockMarketDataSource(size_t event_count = 1000, uint32_t token = 3045);
    ~MockMarketDataSource() override;

    MockMarketDataSource(const MockMarketDataSource&) = delete;
    MockMarketDataSource& operator=(const MockMarketDataSource&) = delete;

    void set_event_callback(EventCallback cb) override { event_callback_ = std::move(cb); }
    void attach_pipeline(MarketDataPipeline* pipeline) { pipeline_ = pipeline; }

    bool start() override;
    void stop() override;
    [[nodiscard]] bool running() const noexcept override { return running_.load(std::memory_order_relaxed); }

    static std::vector<MarketEvent> generate_events(size_t count, uint32_t token = 3045, uint64_t seed = 0x12345678ULL);

private:
    void worker_loop();

    size_t event_count_{1000};
    uint32_t token_{3045};
    EventCallback event_callback_;
    MarketDataPipeline* pipeline_{nullptr};
    std::atomic<bool> running_{false};
    std::thread worker_thread_;
};

// --- Replay Market Data Source (Plays from .mktlog) ---
class ReplayMarketDataSource : public IMarketDataSource {
public:
    explicit ReplayMarketDataSource(std::string mktlog_path);
    ~ReplayMarketDataSource() override;

    ReplayMarketDataSource(const ReplayMarketDataSource&) = delete;
    ReplayMarketDataSource& operator=(const ReplayMarketDataSource&) = delete;

    void set_event_callback(EventCallback cb) override { event_callback_ = std::move(cb); }
    void attach_pipeline(MarketDataPipeline* pipeline) { pipeline_ = pipeline; }

    bool start() override;
    void stop() override;
    [[nodiscard]] bool running() const noexcept override { return running_.load(std::memory_order_relaxed); }

private:
    void worker_loop();

    std::string path_;
    EventCallback event_callback_;
    MarketDataPipeline* pipeline_{nullptr};
    std::atomic<bool> running_{false};
    std::thread worker_thread_;
};

// --- Yahoo Finance Configuration & Parser ---
struct YahooConfig {
    std::vector<std::string> symbols{"RELIANCE.NS", "TCS.NS", "AAPL"};
    uint32_t poll_interval_ms{1000};
    std::string user_agent{"Mozilla/5.0 (Windows NT 10.0; Win64; x64)"};
};

class YahooParser {
public:
    // Parses a single Yahoo Finance v8 chart JSON payload into a normalized MarketEvent.
    // Returns true on success, false on error/invalid input without crashing or throwing.
    static bool parse(std::string_view json, MarketEvent& out_event, uint64_t receive_ts_ns = 0) noexcept;

    // Deterministic FNV-1a 32-bit hash for ticker symbols
    static uint32_t symbol_hash(std::string_view sym) noexcept;
};

// --- Yahoo Finance Market Data Source (External Dev/Test Provider) ---
class YahooMarketDataSource : public IMarketDataSource {
public:
    explicit YahooMarketDataSource(YahooConfig config = {});
    YahooMarketDataSource(std::string symbol, uint32_t poll_interval_ms)
        : YahooMarketDataSource(YahooConfig{{std::move(symbol)}, poll_interval_ms}) {}
    ~YahooMarketDataSource() override;

    YahooMarketDataSource(const YahooMarketDataSource&) = delete;
    YahooMarketDataSource& operator=(const YahooMarketDataSource&) = delete;

    void set_event_callback(EventCallback cb) override { event_callback_ = std::move(cb); }
    void attach_pipeline(MarketDataPipeline* pipeline) { pipeline_ = pipeline; }

    bool start() override;
    void stop() override;
    [[nodiscard]] bool running() const noexcept override { return running_.load(std::memory_order_relaxed); }

    // Synchronously polls configured symbols once; useful for CLI, testing, and inspection
    bool poll_once();

    // Internal fetcher using native system curl (zero third-party dependencies)
    static std::string fetch_symbol_http(const std::string& symbol, const std::string& user_agent);

private:
    void worker_loop();

    YahooConfig config_;
    EventCallback event_callback_;
    MarketDataPipeline* pipeline_{nullptr};
    std::atomic<bool> running_{false};
    std::thread worker_thread_;
    std::atomic<uint64_t> sequence_{0};
};

// ============================================================================
// 5. Binary Recorder and Replayer (.mktlog)
// ============================================================================

class MarketEventRecorder {
public:
    static constexpr size_t DEFAULT_BUFFER_SIZE = 65536; // 64 KB

    explicit MarketEventRecorder(size_t buffer_size = DEFAULT_BUFFER_SIZE);
    ~MarketEventRecorder();

    MarketEventRecorder(const MarketEventRecorder&) = delete;
    MarketEventRecorder& operator=(const MarketEventRecorder&) = delete;
    MarketEventRecorder(MarketEventRecorder&& other) noexcept;
    MarketEventRecorder& operator=(MarketEventRecorder&& other) noexcept;

    bool open(const std::string& path);
    bool write(const MarketEvent& ev) noexcept;
    void flush();
    void close();

    [[nodiscard]] bool is_open() const noexcept { return file_ != nullptr; }
    [[nodiscard]] uint64_t events_written() const noexcept { return events_written_; }
    [[nodiscard]] uint32_t data_crc32() const noexcept { return data_crc32_; }

private:
    FILE* file_{nullptr};
    std::string file_path_;
    std::vector<uint8_t> buffer_;
    size_t buffer_pos_{0};
    uint64_t events_written_{0};
    uint32_t data_crc32_{0};
};

class MarketEventReplayer {
public:
    static constexpr size_t DEFAULT_BUFFER_SIZE = 65536; // 64 KB

    explicit MarketEventReplayer(size_t buffer_size = DEFAULT_BUFFER_SIZE);
    ~MarketEventReplayer();

    MarketEventReplayer(const MarketEventReplayer&) = delete;
    MarketEventReplayer& operator=(const MarketEventReplayer&) = delete;
    MarketEventReplayer(MarketEventReplayer&& other) noexcept;
    MarketEventReplayer& operator=(MarketEventReplayer&& other) noexcept;

    bool open(const std::string& path, std::string* error_out = nullptr);
    bool next(MarketEvent& ev) noexcept;
    bool validate_full_checksum(std::string* error_out = nullptr);
    void close();

    [[nodiscard]] bool is_open() const noexcept { return file_ != nullptr; }
    [[nodiscard]] const MarketFileHeader& header() const noexcept { return header_; }
    [[nodiscard]] uint64_t events_read() const noexcept { return events_read_; }

private:
    FILE* file_{nullptr};
    std::string file_path_;
    MarketFileHeader header_{};
    std::vector<uint8_t> buffer_;
    size_t buffer_pos_{0};
    size_t buffer_valid_{0};
    uint64_t events_read_{0};
    bool eof_reached_{false};
};

// ============================================================================
// 6. Market Data Pipeline
// ============================================================================

class MarketDataPipeline {
public:
    static constexpr size_t DEFAULT_QUEUE_CAPACITY = 16384;

    using MarketEventListener = std::function<void(const MarketEvent&)>;

    explicit MarketDataPipeline(size_t capacity = DEFAULT_QUEUE_CAPACITY);
    ~MarketDataPipeline();

    MarketDataPipeline(const MarketDataPipeline&) = delete;
    MarketDataPipeline& operator=(const MarketDataPipeline&) = delete;
    MarketDataPipeline(MarketDataPipeline&&) = delete;
    MarketDataPipeline& operator=(MarketDataPipeline&&) = delete;

    void start();
    void stop_and_join();

    bool enqueue_event(const MarketEvent& ev) noexcept;
    void enqueue_event_wait(const MarketEvent& ev) noexcept;

    void set_event_listener(MarketEventListener listener) {
        event_listener_ = std::move(listener);
    }

    [[nodiscard]] uint64_t total_enqueued() const noexcept { return total_enqueued_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t total_consumed() const noexcept { return total_consumed_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t total_dropped() const noexcept { return total_dropped_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::optional<MarketEvent> latest_event() const noexcept;

private:
    void consumer_loop();

    std::unique_ptr<SpscQueue<MarketEvent, DEFAULT_QUEUE_CAPACITY, true>> queue_;
    std::thread consumer_thread_;
    std::atomic<bool> running_{false};
    MarketEventListener event_listener_;

    alignas(64) std::atomic<uint64_t> total_enqueued_{0};
    alignas(64) std::atomic<uint64_t> total_consumed_{0};
    alignas(64) std::atomic<uint64_t> total_dropped_{0};

    mutable std::atomic<bool> has_latest_{false};
    alignas(64) MarketEvent latest_event_{};
};

} // namespace hft
