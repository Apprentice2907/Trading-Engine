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

// ============================================================================
// 4. Angel One SmartAPI SmartStream Definitions
// ============================================================================

namespace broker {

struct AngelConstants {
    static constexpr const char* DEFAULT_WS_HOST = "smartapisocket.angelone.in";
    static constexpr const char* DEFAULT_WS_PATH = "/smart-stream";
    static constexpr uint16_t    DEFAULT_WS_PORT = 443;

    static constexpr uint8_t MODE_LTP        = 1;
    static constexpr uint8_t MODE_QUOTE      = 2;
    static constexpr uint8_t MODE_SNAP_QUOTE = 3;
    static constexpr uint8_t MODE_DEPTH      = 4;

    static constexpr size_t PACKET_SIZE_LTP        = 51;
    static constexpr size_t PACKET_SIZE_QUOTE      = 147;
    static constexpr size_t PACKET_SIZE_SNAP_QUOTE = 347;

    static constexpr uint8_t EXCH_NSE_CM = 1;
    static constexpr uint8_t EXCH_NSE_FO = 2;
    static constexpr uint8_t EXCH_BSE_CM = 3;
    static constexpr uint8_t EXCH_BSE_FO = 4;
    static constexpr uint8_t EXCH_MCX_FO = 5;
    static constexpr uint8_t EXCH_NCX_FO = 7;
    static constexpr uint8_t EXCH_CDE_FO = 13;
};

enum class ConnectionState : uint8_t {
    Disconnected = 0,
    Connecting,
    Connected,
    Subscribed,
    Reconnecting,
    Error
};

struct AngelConfig {
    std::string api_key;
    std::string client_code;
    std::string feed_token;
    std::string jwt_token;

    uint32_t instrument_token{3045};
    uint8_t  exchange_type{AngelConstants::EXCH_NSE_CM};
    uint8_t  subscription_mode{AngelConstants::MODE_QUOTE};

    uint32_t ping_interval_sec{10};
    uint32_t reconnect_delay_ms{2000};
    uint32_t max_reconnect_attempts{5};

    std::string host{AngelConstants::DEFAULT_WS_HOST};
    std::string path{AngelConstants::DEFAULT_WS_PATH};
    uint16_t    port{AngelConstants::DEFAULT_WS_PORT};
};

struct BrokerStats {
    std::atomic<uint64_t> received_packets{0};
    std::atomic<uint64_t> decoded_events{0};
    std::atomic<uint64_t> normalized_events{0};
    std::atomic<uint64_t> queued_events{0};
    std::atomic<uint64_t> dropped_events{0};

    void reset() noexcept {
        received_packets.store(0, std::memory_order_relaxed);
        decoded_events.store(0, std::memory_order_relaxed);
        normalized_events.store(0, std::memory_order_relaxed);
        queued_events.store(0, std::memory_order_relaxed);
        dropped_events.store(0, std::memory_order_relaxed);
    }
};

class AngelDecoder {
public:
    static bool decode(const uint8_t* data, size_t length, MarketEvent& out_event,
                       uint64_t receive_ts_ns) noexcept;
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

class AngelClient {
public:
    using PacketCallback = std::function<void(const uint8_t* data, size_t length)>;

    explicit AngelClient(AngelConfig config);
    ~AngelClient();

    AngelClient(const AngelClient&) = delete;
    AngelClient& operator=(const AngelClient&) = delete;
    AngelClient(AngelClient&& other) noexcept;
    AngelClient& operator=(AngelClient&& other) noexcept;

    bool connect();
    bool subscribe(uint32_t token, uint8_t mode = AngelConstants::MODE_QUOTE,
                   uint8_t exchange = AngelConstants::EXCH_NSE_CM);
    bool send_ping();
    void run_receive_loop();
    void stop();
    void disconnect();

    void set_packet_callback(PacketCallback callback) {
        packet_callback_ = std::move(callback);
    }

    [[nodiscard]] bool is_connected() const noexcept {
        return state_.load(std::memory_order_relaxed) == ConnectionState::Connected ||
               state_.load(std::memory_order_relaxed) == ConnectionState::Subscribed;
    }

    [[nodiscard]] ConnectionState state() const noexcept {
        return state_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] const BrokerStats& stats() const noexcept { return stats_; }
    [[nodiscard]] BrokerStats& stats() noexcept { return stats_; }

    static AngelConfig load_config_from_env();

private:
    void heartbeat_worker();

    AngelConfig config_;
    BrokerStats stats_;
    std::atomic<ConnectionState> state_{ConnectionState::Disconnected};
    std::atomic<bool> running_{false};
    PacketCallback packet_callback_;

    void* h_session_{nullptr};
    void* h_connect_{nullptr};
    void* h_request_{nullptr};
    void* h_websocket_{nullptr};

    std::unique_ptr<std::thread> heartbeat_thread_;
};

} // namespace broker

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
