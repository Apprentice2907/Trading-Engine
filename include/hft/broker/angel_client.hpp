#pragma once

#include "hft/broker/angel_types.hpp"
#include <functional>
#include <string>
#include <atomic>
#include <thread>
#include <memory>

namespace hft::broker {

/**
 * @brief Native Windows WinHttp WebSocket client for Angel One SmartAPI SmartStream.
 *
 * Provides connection management, TLS upgrade, subscription dispatch,
 * ping heartbeat, and raw binary receive loop without third-party dependencies.
 */
class AngelClient {
public:
    using PacketCallback = std::function<void(const uint8_t* data, size_t length)>;

    explicit AngelClient(AngelConfig config);
    ~AngelClient();

    // Non-copyable, movable
    AngelClient(const AngelClient&) = delete;
    AngelClient& operator=(const AngelClient&) = delete;
    AngelClient(AngelClient&& other) noexcept;
    AngelClient& operator=(AngelClient&& other) noexcept;

    /**
     * @brief Connects to Angel One SmartStream WebSocket over TLS.
     * Performs WebSocket upgrade and sets state to Connected.
     *
     * @return true if connected successfully, false on network/auth error
     */
    bool connect();

    /**
     * @brief Sends subscription request for the configured instrument.
     */
    bool subscribe(uint32_t token, uint8_t mode = AngelConstants::MODE_QUOTE,
                   uint8_t exchange = AngelConstants::EXCH_NSE_CM);

    /**
     * @brief Sends a ping heartbeat message to keep connection alive.
     */
    bool send_ping();

    /**
     * @brief Starts the receive loop in the current thread or a background worker.
     */
    void run_receive_loop();

    /**
     * @brief Signals the receive loop and background worker to terminate cleanly.
     */
    void stop();

    /**
     * @brief Closes connection and cleans up handles.
     */
    void disconnect();

    /**
     * @brief Registers callback invoked whenever a binary packet is received.
     */
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

    /**
     * @brief Factory loading credentials and config from environment variables.
     */
    static AngelConfig load_config_from_env();

private:
    void heartbeat_worker();

    AngelConfig config_;
    BrokerStats stats_;
    std::atomic<ConnectionState> state_{ConnectionState::Disconnected};
    std::atomic<bool> running_{false};
    PacketCallback packet_callback_;

    // Native Windows WinHTTP handles (stored as void* to avoid Windows.h in header)
    void* h_session_{nullptr};
    void* h_connect_{nullptr};
    void* h_request_{nullptr};
    void* h_websocket_{nullptr};

    std::unique_ptr<std::thread> heartbeat_thread_;
};

} // namespace hft::broker
