#define _CRT_SECURE_NO_WARNINGS
#include "hft/market_data.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#endif

#include <iostream>
#include <vector>
#include <chrono>
#include <cstdlib>

namespace hft::broker {

namespace {

#ifdef _WIN32
std::wstring to_wide_string(const std::string& str) {
    if (str.empty()) return std::wstring();
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.size()), nullptr, 0);
    std::wstring wstr(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.size()), &wstr[0], size_needed);
    return wstr;
}
#endif

} // namespace

AngelConfig AngelClient::load_config_from_env() {
    AngelConfig cfg;

    const char* api_key = std::getenv("ANGEL_API_KEY");
    if (api_key) cfg.api_key = api_key;

    const char* client_code = std::getenv("ANGEL_CLIENT_CODE");
    if (client_code) cfg.client_code = client_code;

    const char* feed_token = std::getenv("ANGEL_FEED_TOKEN");
    if (feed_token) cfg.feed_token = feed_token;

    const char* jwt_token = std::getenv("ANGEL_JWT_TOKEN");
    if (jwt_token) cfg.jwt_token = jwt_token;

    const char* token_str = std::getenv("ANGEL_INSTRUMENT_TOKEN");
    if (token_str) {
        cfg.instrument_token = static_cast<uint32_t>(std::stoul(token_str));
    }

    return cfg;
}

AngelClient::AngelClient(AngelConfig config)
    : config_(std::move(config)) {}

AngelClient::~AngelClient() {
    disconnect();
}

AngelClient::AngelClient(AngelClient&& other) noexcept
    : config_(std::move(other.config_)),
      state_(other.state_.load()),
      running_(other.running_.load()),
      packet_callback_(std::move(other.packet_callback_)),
      h_session_(other.h_session_),
      h_connect_(other.h_connect_),
      h_request_(other.h_request_),
      h_websocket_(other.h_websocket_) {
    other.h_session_ = nullptr;
    other.h_connect_ = nullptr;
    other.h_request_ = nullptr;
    other.h_websocket_ = nullptr;
}

AngelClient& AngelClient::operator=(AngelClient&& other) noexcept {
    if (this != &other) {
        disconnect();
        config_ = std::move(other.config_);
        state_.store(other.state_.load());
        running_.store(other.running_.load());
        packet_callback_ = std::move(other.packet_callback_);
        h_session_ = other.h_session_;
        h_connect_ = other.h_connect_;
        h_request_ = other.h_request_;
        h_websocket_ = other.h_websocket_;

        other.h_session_ = nullptr;
        other.h_connect_ = nullptr;
        other.h_request_ = nullptr;
        other.h_websocket_ = nullptr;
    }
    return *this;
}

bool AngelClient::connect() {
#ifndef _WIN32
    std::cerr << "AngelClient currently supports native Windows WinHttpWebSocket.\n";
    return false;
#else
    if (config_.api_key.empty() || config_.client_code.empty() || config_.feed_token.empty()) {
        std::cerr << "Error: Angel One credentials missing (ANGEL_API_KEY, ANGEL_CLIENT_CODE, ANGEL_FEED_TOKEN).\n";
        state_.store(ConnectionState::Error);
        return false;
    }

    state_.store(ConnectionState::Connecting);

    h_session_ = WinHttpOpen(L"HFT-TradingEngine/1.0",
                             WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                             WINHTTP_NO_PROXY_NAME,
                             WINHTTP_NO_PROXY_BYPASS,
                             0);
    if (!h_session_) {
        std::cerr << "Failed WinHttpOpen: " << GetLastError() << "\n";
        state_.store(ConnectionState::Error);
        return false;
    }

    std::wstring w_host = to_wide_string(AngelConstants::DEFAULT_WS_HOST);
    h_connect_ = WinHttpConnect(h_session_, w_host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!h_connect_) {
        std::cerr << "Failed WinHttpConnect: " << GetLastError() << "\n";
        disconnect();
        return false;
    }

    std::wstring w_path = to_wide_string(AngelConstants::DEFAULT_WS_PATH);
    h_request_ = WinHttpOpenRequest(h_connect_,
                                    L"GET",
                                    w_path.c_str(),
                                    nullptr,
                                    WINHTTP_NO_REFERER,
                                    WINHTTP_DEFAULT_ACCEPT_TYPES,
                                    WINHTTP_FLAG_SECURE);
    if (!h_request_) {
        std::cerr << "Failed WinHttpOpenRequest: " << GetLastError() << "\n";
        disconnect();
        return false;
    }

    // Enable WebSocket Upgrade
    if (!WinHttpSetOption(h_request_, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0)) {
        std::cerr << "Failed WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET: " << GetLastError() << "\n";
        disconnect();
        return false;
    }

    // Set Angel One SmartStream Authentication Headers
    std::string headers;
    if (!config_.jwt_token.empty()) {
        headers += "Authorization: Bearer " + config_.jwt_token + "\r\n";
    }
    headers += "x-api-key: " + config_.api_key + "\r\n";
    headers += "x-client-code: " + config_.client_code + "\r\n";
    headers += "x-feed-token: " + config_.feed_token + "\r\n";

    std::wstring w_headers = to_wide_string(headers);
    if (!WinHttpAddRequestHeaders(h_request_, w_headers.c_str(), static_cast<DWORD>(-1L), WINHTTP_ADDREQ_FLAG_ADD)) {
        std::cerr << "Failed WinHttpAddRequestHeaders: " << GetLastError() << "\n";
        disconnect();
        return false;
    }

    if (!WinHttpSendRequest(h_request_, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        std::cerr << "Failed WinHttpSendRequest: " << GetLastError() << "\n";
        disconnect();
        return false;
    }

    if (!WinHttpReceiveResponse(h_request_, nullptr)) {
        std::cerr << "Failed WinHttpReceiveResponse: " << GetLastError() << "\n";
        disconnect();
        return false;
    }

    DWORD status_code = 0;
    DWORD status_size = sizeof(status_code);
    if (!WinHttpQueryHeaders(h_request_,
                             WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX,
                             &status_code,
                             &status_size,
                             WINHTTP_NO_HEADER_INDEX)) {
        std::cerr << "Failed WinHttpQueryHeaders: " << GetLastError() << "\n";
        disconnect();
        return false;
    }

    if (status_code != 101) {
        std::cerr << "WebSocket upgrade rejected by Angel One server with status: " << status_code << "\n";
        disconnect();
        return false;
    }

    h_websocket_ = WinHttpWebSocketCompleteUpgrade(h_request_, 0);
    if (!h_websocket_) {
        std::cerr << "Failed WinHttpWebSocketCompleteUpgrade: " << GetLastError() << "\n";
        disconnect();
        return false;
    }

    WinHttpCloseHandle(h_request_);
    h_request_ = nullptr;

    state_.store(ConnectionState::Connected);
    running_.store(true);

    // Start background ping heartbeat thread
    heartbeat_thread_ = std::make_unique<std::thread>(&AngelClient::heartbeat_worker, this);

    return true;
#endif
}

bool AngelClient::subscribe(uint32_t token, uint8_t mode, uint8_t exchange) {
#ifndef _WIN32
    return false;
#else
    if (!h_websocket_) return false;

    std::string sub_req = "{\"action\": 1, \"params\": {\"mode\": " +
                          std::to_string(mode) +
                          ", \"tokenList\": [{\"exchangeType\": " +
                          std::to_string(exchange) +
                          ", \"tokens\": [\"" +
                          std::to_string(token) +
                          "\"]}]}}";

    DWORD err = WinHttpWebSocketSend(h_websocket_,
                                     WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                                     const_cast<char*>(sub_req.data()),
                                     static_cast<DWORD>(sub_req.size()));
    if (err != ERROR_SUCCESS) {
        std::cerr << "Failed WinHttpWebSocketSend for subscribe: " << err << "\n";
        return false;
    }

    state_.store(ConnectionState::Subscribed);
    return true;
#endif
}

bool AngelClient::send_ping() {
#ifndef _WIN32
    return false;
#else
    if (!h_websocket_) return false;

    std::string ping_msg = "ping";
    DWORD err = WinHttpWebSocketSend(h_websocket_,
                                     WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                                     const_cast<char*>(ping_msg.data()),
                                     static_cast<DWORD>(ping_msg.size()));
    return (err == ERROR_SUCCESS);
#endif
}

void AngelClient::heartbeat_worker() {
    while (running_.load(std::memory_order_relaxed)) {
        for (uint32_t i = 0; i < config_.ping_interval_sec && running_.load(std::memory_order_relaxed); ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        if (running_.load(std::memory_order_relaxed) && is_connected()) {
            send_ping();
        }
    }
}

void AngelClient::run_receive_loop() {
#ifndef _WIN32
    return;
#else
    if (!h_websocket_) return;

    std::vector<uint8_t> buffer(4096);

    while (running_.load(std::memory_order_relaxed)) {
        DWORD bytes_transferred = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE buffer_type;

        DWORD dw_err = WinHttpWebSocketReceive(h_websocket_,
                                              buffer.data(),
                                              static_cast<DWORD>(buffer.size()),
                                              &bytes_transferred,
                                              &buffer_type);
        if (dw_err != ERROR_SUCCESS) {
            if (running_.load(std::memory_order_relaxed)) {
                std::cerr << "WinHttpWebSocketReceive error: " << dw_err << "\n";
                state_.store(ConnectionState::Error);
            }
            break;
        }

        if (buffer_type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE) {
            stats_.received_packets.fetch_add(1, std::memory_order_relaxed);
            if (packet_callback_) {
                packet_callback_(buffer.data(), bytes_transferred);
            }
        } else if (buffer_type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
            std::cout << "Angel One SmartStream closed by server.\n";
            state_.store(ConnectionState::Disconnected);
            break;
        }
    }
#endif
}

void AngelClient::stop() {
    running_.store(false);
#ifdef _WIN32
    if (h_websocket_) {
        WinHttpWebSocketShutdown(h_websocket_, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
    }
#endif
    if (heartbeat_thread_ && heartbeat_thread_->joinable()) {
        heartbeat_thread_->join();
        heartbeat_thread_.reset();
    }
}

void AngelClient::disconnect() {
    stop();
#ifdef _WIN32
    if (h_websocket_) {
        WinHttpWebSocketClose(h_websocket_, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
        WinHttpCloseHandle(h_websocket_);
        h_websocket_ = nullptr;
    }
    if (h_request_) {
        WinHttpCloseHandle(h_request_);
        h_request_ = nullptr;
    }
    if (h_connect_) {
        WinHttpCloseHandle(h_connect_);
        h_connect_ = nullptr;
    }
    if (h_session_) {
        WinHttpCloseHandle(h_session_);
        h_session_ = nullptr;
    }
#endif
    state_.store(ConnectionState::Disconnected);
}

} // namespace hft::broker
