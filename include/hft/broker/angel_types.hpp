#pragma once

#include <cstdint>
#include <string>
#include <atomic>

namespace hft::broker {

/**
 * @brief Official Angel One SmartAPI SmartStream WebSocket 2.0 constants.
 */
struct AngelConstants {
    static constexpr const char* DEFAULT_WS_HOST = "smartapisocket.angelone.in";
    static constexpr const char* DEFAULT_WS_PATH = "/smart-stream";
    static constexpr uint16_t    DEFAULT_WS_PORT = 443;

    // Subscription Modes
    static constexpr uint8_t MODE_LTP        = 1;
    static constexpr uint8_t MODE_QUOTE      = 2;
    static constexpr uint8_t MODE_SNAP_QUOTE = 3;
    static constexpr uint8_t MODE_DEPTH      = 4;

    // Packet Sizes (in bytes)
    static constexpr size_t PACKET_SIZE_LTP        = 51;
    static constexpr size_t PACKET_SIZE_QUOTE      = 147;
    static constexpr size_t PACKET_SIZE_SNAP_QUOTE = 347;

    // Exchange Types
    static constexpr uint8_t EXCH_NSE_CM = 1; // Cash Market (Equities)
    static constexpr uint8_t EXCH_NSE_FO = 2; // Futures & Options
    static constexpr uint8_t EXCH_BSE_CM = 3; // BSE Equities
    static constexpr uint8_t EXCH_BSE_FO = 4; // BSE Derivatives
    static constexpr uint8_t EXCH_MCX_FO = 5; // Multi Commodity Exchange
    static constexpr uint8_t EXCH_NCX_FO = 7; // NCDEX
    static constexpr uint8_t EXCH_CDE_FO = 13;// Currency Derivatives
};

/**
 * @brief Connection state of the broker adapter.
 */
enum class ConnectionState : uint8_t {
    Disconnected = 0,
    Connecting,
    Connected,
    Subscribed,
    Reconnecting,
    Error
};

/**
 * @brief Configuration parameters for Angel One SmartAPI connection.
 */
struct AngelConfig {
    std::string api_key;
    std::string client_code;
    std::string feed_token;
    std::string jwt_token;

    uint32_t instrument_token{3045}; // Default: SBIN-EQ
    uint8_t  exchange_type{AngelConstants::EXCH_NSE_CM};
    uint8_t  subscription_mode{AngelConstants::MODE_QUOTE};

    uint32_t ping_interval_sec{10};
    uint32_t reconnect_delay_ms{2000};
    uint32_t max_reconnect_attempts{5};
};

/**
 * @brief Atomic runtime statistics for monitoring data flow and backpressure.
 */
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

} // namespace hft::broker
