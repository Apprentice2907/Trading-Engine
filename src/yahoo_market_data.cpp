#define _CRT_SECURE_NO_WARNINGS

#include "hft/market_data.hpp"

#include <iostream>
#include <sstream>
#include <chrono>
#include <cmath>
#include <charconv>
#include <cstring>
#include <cstdio>
#include <array>

#ifdef _WIN32
#define POPEN_FUNC _popen
#define PCLOSE_FUNC _pclose
#else
#define POPEN_FUNC popen
#define PCLOSE_FUNC pclose
#endif

namespace hft {

namespace {

// Lightweight helper to find a JSON key and extract its value span without any dynamic heap allocations
bool find_json_key(std::string_view json, std::string_view key, std::string_view& value_out) noexcept {
    size_t search_pos = 0;
    while (search_pos < json.size()) {
        size_t pos = json.find(key, search_pos);
        if (pos == std::string_view::npos) return false;

        // Check if preceded and followed by double quotes: "key"
        if (pos > 0 && json[pos - 1] == '"' && (pos + key.size()) < json.size() && json[pos + key.size()] == '"') {
            size_t val_pos = pos + key.size() + 1; // position immediately after closing quote
            while (val_pos < json.size() && (json[val_pos] == ' ' || json[val_pos] == '\t' ||
                   json[val_pos] == '\r' || json[val_pos] == '\n' || json[val_pos] == ':')) {
                ++val_pos;
            }
            if (val_pos >= json.size()) return false;

            if (json[val_pos] == '"') {
                // Quoted string
                ++val_pos;
                size_t end = json.find('"', val_pos);
                if (end == std::string_view::npos) return false;
                value_out = json.substr(val_pos, end - val_pos);
                return true;
            } else {
                // Number or boolean or null
                size_t end = val_pos;
                while (end < json.size() && json[end] != ',' && json[end] != '}' && json[end] != ']' &&
                       json[end] != ' ' && json[end] != '\r' && json[end] != '\n' && json[end] != '\t') {
                    ++end;
                }
                value_out = json.substr(val_pos, end - val_pos);
                return true;
            }
        }
        search_pos = pos + 1;
    }
    return false;
}

// Parse double from string_view safely
bool parse_double_fast(std::string_view s, double& out) noexcept {
    if (s.empty()) return false;
    // Check for null or non-numeric tokens
    if (s == "null" || s == "NaN" || s == "Infinity" || s == "-Infinity") return false;

    char buf[64]{0};
    if (s.size() >= sizeof(buf)) return false;
    std::memcpy(buf, s.data(), s.size());
    buf[s.size()] = '\0';

    char* endptr = nullptr;
    double val = std::strtod(buf, &endptr);
    if (endptr == buf || *endptr != '\0') return false;
    if (std::isnan(val) || std::isinf(val)) return false;

    out = val;
    return true;
}

// Parse int64 from string_view safely
bool parse_int64_fast(std::string_view s, int64_t& out) noexcept {
    if (s.empty() || s == "null") return false;

    char buf[32]{0};
    if (s.size() >= sizeof(buf)) return false;
    std::memcpy(buf, s.data(), s.size());
    buf[s.size()] = '\0';

    char* endptr = nullptr;
    long long val = std::strtoll(buf, &endptr, 10);
    if (endptr == buf || *endptr != '\0') return false;

    out = static_cast<int64_t>(val);
    return true;
}

} // namespace

// ============================================================================
// YahooParser Implementation
// ============================================================================

uint32_t YahooParser::symbol_hash(std::string_view sym) noexcept {
    return crc32(0, sym.data(), sym.size()) & 0x7FFFFFFFU;
}

bool YahooParser::parse(std::string_view json, MarketEvent& out_event, uint64_t receive_ts_ns) noexcept {
    if (json.empty()) return false;

    // Check for error block
    std::string_view err_val;
    if (find_json_key(json, "error", err_val) && !err_val.empty() && err_val != "null") {
        return false;
    }

    // 1. Symbol (Required)
    std::string_view symbol;
    if (!find_json_key(json, "symbol", symbol) || symbol.empty()) {
        return false;
    }

    // 2. Regular Market Price (Required)
    std::string_view price_str;
    if (!find_json_key(json, "regularMarketPrice", price_str) || price_str.empty()) {
        return false;
    }
    double price_val = 0.0;
    if (!parse_double_fast(price_str, price_val) || price_val <= 0.0) {
        return false;
    }

    // 3. Regular Market Time (Optional, default to 0)
    int64_t time_sec = 0;
    std::string_view time_str;
    if (find_json_key(json, "regularMarketTime", time_str)) {
        (void)parse_int64_fast(time_str, time_sec);
    }

    // 4. Volume (Optional, default to 0)
    int64_t volume_val = 0;
    std::string_view vol_str;
    if (find_json_key(json, "regularMarketVolume", vol_str)) {
        (void)parse_int64_fast(vol_str, volume_val);
    }
    if (volume_val < 0) volume_val = 0;

    // 5. Exchange identifier
    uint8_t exch = exchange::UNKNOWN;
    std::string_view exch_name;
    if (find_json_key(json, "exchangeName", exch_name) || find_json_key(json, "fullExchangeName", exch_name)) {
        if (exch_name.find("NSE") != std::string_view::npos || exch_name.find("NSI") != std::string_view::npos) {
            exch = exchange::NSE;
        } else if (exch_name.find("BSE") != std::string_view::npos) {
            exch = exchange::BSE;
        } else if (exch_name.find("Nasdaq") != std::string_view::npos || exch_name.find("NMS") != std::string_view::npos) {
            exch = exchange::NASDAQ;
        }
    }

    // Determine receive timestamp
    uint64_t now_ns = receive_ts_ns;
    if (now_ns == 0) {
        now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // Populate normalized MarketEvent
    std::memset(&out_event, 0, sizeof(out_event));
    out_event.instrument_token = symbol_hash(symbol);
    out_event.exchange_type = exch;
    out_event.subscription_mode = 1; // Quote / Trade
    out_event.pad = 0;
    out_event.sequence_number = 1;
    out_event.exchange_timestamp = (time_sec > 0) ? static_cast<uint64_t>(time_sec) * 1000000000ULL : 0ULL;
    out_event.receive_timestamp = now_ns;
    out_event.last_price = static_cast<int64_t>(std::round(price_val * 100.0));
    out_event.last_quantity = 0;      // Not provided in basic chart feed; do not fabricate
    out_event.best_bid_price = 0;     // Not provided in basic chart feed; do not fabricate
    out_event.best_bid_quantity = 0;  // Not provided in basic chart feed; do not fabricate
    out_event.best_ask_price = 0;     // Not provided in basic chart feed; do not fabricate
    out_event.best_ask_quantity = 0;  // Not provided in basic chart feed; do not fabricate
    out_event.volume = static_cast<uint64_t>(volume_val);

    // Embed symbol string into reserved area for transparent diagnostic reporting
    size_t copy_len = std::min(symbol.size(), sizeof(out_event.reserved) - 1);
    std::memcpy(out_event.reserved, symbol.data(), copy_len);
    out_event.reserved[copy_len] = '\0';

    return true;
}

// ============================================================================
// YahooMarketDataSource Implementation
// ============================================================================

YahooMarketDataSource::YahooMarketDataSource(YahooConfig config)
    : config_(std::move(config)) {}

YahooMarketDataSource::~YahooMarketDataSource() {
    stop();
}

std::string YahooMarketDataSource::fetch_symbol_http(const std::string& symbol, const std::string& user_agent) {
    std::string url = "https://query1.finance.yahoo.com/v8/finance/chart/" + symbol + "?interval=1d&range=1d";
    std::string cmd = "curl -s -A \"" + user_agent + "\" --max-time 10 \"" + url + "\"";

    FILE* pipe = POPEN_FUNC(cmd.c_str(), "r");
    if (!pipe) {
        return "";
    }

    std::array<char, 4096> buffer;
    std::string result;
    while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        result.append(buffer.data());
    }
    PCLOSE_FUNC(pipe);
    return result;
}

bool YahooMarketDataSource::poll_once() {
    bool any_success = false;
    for (const auto& symbol : config_.symbols) {
        std::string json = fetch_symbol_http(symbol, config_.user_agent);
        if (json.empty()) continue;

        MarketEvent ev{};
        if (YahooParser::parse(json, ev)) {
            ev.sequence_number = sequence_.fetch_add(1, std::memory_order_relaxed) + 1;
            if (pipeline_) {
                pipeline_->enqueue_event(ev);
            }
            if (event_callback_) {
                event_callback_(ev);
            }
            any_success = true;
        }
    }
    return any_success;
}

bool YahooMarketDataSource::start() {
    if (running_.exchange(true)) return false;
    worker_thread_ = std::thread(&YahooMarketDataSource::worker_loop, this);
    return true;
}

void YahooMarketDataSource::stop() {
    if (running_.exchange(false)) {
        if (worker_thread_.joinable()) {
            worker_thread_.join();
        }
    }
}

void YahooMarketDataSource::worker_loop() {
    while (running_.load(std::memory_order_relaxed)) {
        poll_once();
        std::this_thread::sleep_for(std::chrono::milliseconds(config_.poll_interval_ms));
    }
}

} // namespace hft
