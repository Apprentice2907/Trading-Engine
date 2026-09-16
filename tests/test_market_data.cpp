#define _CRT_SECURE_NO_WARNINGS
#include "test_framework.hpp"
#include "hft/market_data.hpp"

#include <vector>
#include <cstdio>
#include <filesystem>
#include <chrono>

using namespace hft;

namespace {

std::vector<MarketEvent> generate_deterministic_market_events(size_t count, uint64_t seed = 0x12345678ULL) {
    return MockMarketDataSource::generate_events(count, 3045, seed);
}

} // namespace

// ============================================================================
// 1. MarketEvent Layout & Alignment
// ============================================================================

TEST_CASE(MarketEvent_LayoutAndAlignment) {
    ASSERT_EQ(sizeof(MarketEvent), 128);
    ASSERT_EQ(alignof(MarketEvent), 64);
    ASSERT_TRUE(std::is_trivially_copyable_v<MarketEvent>);
    ASSERT_TRUE(std::is_standard_layout_v<MarketEvent>);
}

// ============================================================================
// 2. Yahoo Finance Market Data Parser & Mock Provider Tests
// ============================================================================

TEST_CASE(YahooParser_ValidUSQuote) {
    const std::string json = R"({
        "chart": {
            "result": [{
                "meta": {
                    "currency": "USD",
                    "symbol": "AAPL",
                    "exchangeName": "NMS",
                    "instrumentType": "EQUITY",
                    "regularMarketTime": 1710000000,
                    "regularMarketPrice": 185.50,
                    "regularMarketVolume": 45000000
                }
            }],
            "error": null
        }
    })";

    MarketEvent ev{};
    uint64_t recv_ts = 987654321ULL;
    bool ok = YahooParser::parse(json, ev, recv_ts);

    ASSERT_TRUE(ok);
    ASSERT_EQ(ev.instrument_token, YahooParser::symbol_hash("AAPL"));
    ASSERT_EQ(ev.exchange_type, 2); // NMS/US
    ASSERT_EQ(ev.subscription_mode, 1);
    ASSERT_EQ(ev.sequence_number, 1);
    ASSERT_EQ(ev.exchange_timestamp, 1710000000ULL * 1000000000ULL);
    ASSERT_EQ(ev.receive_timestamp, recv_ts);
    ASSERT_EQ(ev.last_price, 18550); // 185.50 * 100
    ASSERT_EQ(ev.volume, 45000000);
    // Explicitly check zero-bid/ask / no fabrication
    ASSERT_EQ(ev.last_quantity, 0);
    ASSERT_EQ(ev.best_bid_price, 0);
    ASSERT_EQ(ev.best_bid_quantity, 0);
    ASSERT_EQ(ev.best_ask_price, 0);
    ASSERT_EQ(ev.best_ask_quantity, 0);
    // Verify symbol in reserved field
    ASSERT_TRUE(std::string_view(reinterpret_cast<const char*>(ev.reserved)).find("AAPL") != std::string_view::npos);
}

TEST_CASE(YahooParser_ValidIndianQuote) {
    const std::string json = R"({
        "chart": {
            "result": [{
                "meta": {
                    "currency": "INR",
                    "symbol": "RELIANCE.NS",
                    "exchangeName": "NSE",
                    "regularMarketTime": 1710001000,
                    "regularMarketPrice": 2985.75,
                    "regularMarketVolume": 8500000
                }
            }],
            "error": null
        }
    })";

    MarketEvent ev{};
    bool ok = YahooParser::parse(json, ev, 11223344ULL);

    ASSERT_TRUE(ok);
    ASSERT_EQ(ev.instrument_token, YahooParser::symbol_hash("RELIANCE.NS"));
    ASSERT_EQ(ev.exchange_type, 1); // NSE/India
    ASSERT_EQ(ev.last_price, 298575); // 2985.75 * 100
    ASSERT_EQ(ev.volume, 8500000);
    ASSERT_EQ(ev.best_bid_price, 0);
    ASSERT_EQ(ev.best_ask_price, 0);
}

TEST_CASE(YahooParser_MissingPrice) {
    const std::string json = R"({
        "chart": {
            "result": [{
                "meta": {
                    "symbol": "AAPL",
                    "regularMarketVolume": 10000
                }
            }]
        }
    })";

    MarketEvent ev{};
    ASSERT_FALSE(YahooParser::parse(json, ev, 0));
}

TEST_CASE(YahooParser_MissingSymbol) {
    const std::string json = R"({
        "chart": {
            "result": [{
                "meta": {
                    "regularMarketPrice": 150.00,
                    "regularMarketVolume": 10000
                }
            }]
        }
    })";

    MarketEvent ev{};
    ASSERT_FALSE(YahooParser::parse(json, ev, 0));
}

TEST_CASE(YahooParser_InvalidNumeric) {
    const std::string json = R"({
        "chart": {
            "result": [{
                "meta": {
                    "symbol": "AAPL",
                    "regularMarketPrice": "NOT_A_NUMBER"
                }
            }]
        }
    })";

    MarketEvent ev{};
    ASSERT_FALSE(YahooParser::parse(json, ev, 0));
}

TEST_CASE(YahooParser_MalformedJson) {
    MarketEvent ev{};
    ASSERT_FALSE(YahooParser::parse("", ev, 0));
    ASSERT_FALSE(YahooParser::parse("   ", ev, 0));
    ASSERT_FALSE(YahooParser::parse("{\"chart\": { truncated...", ev, 0));
    ASSERT_FALSE(YahooParser::parse("random non-json garbage data", ev, 0));
}

TEST_CASE(YahooParser_ErrorResponse) {
    const std::string json = R"({
        "chart": {
            "result": null,
            "error": {
                "code": "Not Found",
                "description": "No data found for symbol XYZ"
            }
        }
    })";

    MarketEvent ev{};
    ASSERT_FALSE(YahooParser::parse(json, ev, 0));
}

TEST_CASE(YahooParser_ZeroBidAskEnforcement) {
    const std::string json = R"({
        "chart": {
            "result": [{
                "meta": {
                    "symbol": "MSFT",
                    "regularMarketPrice": 420.10,
                    "regularMarketVolume": 123456
                }
            }]
        }
    })";

    MarketEvent ev{};
    ASSERT_TRUE(YahooParser::parse(json, ev, 12345));
    ASSERT_EQ(ev.best_bid_price, 0);
    ASSERT_EQ(ev.best_bid_quantity, 0);
    ASSERT_EQ(ev.best_ask_price, 0);
    ASSERT_EQ(ev.best_ask_quantity, 0);
    ASSERT_EQ(ev.last_quantity, 0);
}

TEST_CASE(MockMarketDataSource_DeterministicGeneration) {
    auto events = MockMarketDataSource::generate_events(500, 3045, 0xABCDEFULL);
    ASSERT_EQ(events.size(), 500u);

    for (size_t i = 0; i < events.size(); ++i) {
        ASSERT_EQ(events[i].instrument_token, 3045u);
        ASSERT_EQ(events[i].sequence_number, i + 1);
        ASSERT_TRUE(events[i].last_price > 0);
        ASSERT_TRUE(events[i].best_bid_price < events[i].best_ask_price);
        ASSERT_TRUE(events[i].best_bid_quantity > 0);
        ASSERT_TRUE(events[i].best_ask_quantity > 0);
        if (i > 0) {
            ASSERT_TRUE(events[i].exchange_timestamp >= events[i - 1].exchange_timestamp);
            ASSERT_TRUE(events[i].receive_timestamp >= events[i - 1].receive_timestamp);
        }
    }
}

// ============================================================================
// 3. Market Data Pipeline & Concurrency
// ============================================================================

TEST_CASE(MarketData_SpscThreadTransfer) {
    MarketDataPipeline pipeline;
    std::vector<MarketEvent> consumed_events;
    consumed_events.reserve(5000);

    pipeline.set_event_listener([&consumed_events](const MarketEvent& ev) {
        consumed_events.push_back(ev);
    });

    pipeline.start();

    const size_t event_count = 5000;
    for (size_t i = 0; i < event_count; ++i) {
        MarketEvent ev{};
        ev.instrument_token = 3045;
        ev.sequence_number = i + 1;
        ev.last_price = 83000 + static_cast<int64_t>(i % 50);
        ev.last_quantity = 10;
        pipeline.enqueue_event_wait(ev);
    }

    pipeline.stop_and_join();

    ASSERT_EQ(consumed_events.size(), event_count);
    ASSERT_EQ(pipeline.total_consumed(), event_count);

    for (size_t i = 0; i < event_count; ++i) {
        ASSERT_EQ(consumed_events[i].sequence_number, i + 1);
        ASSERT_EQ(consumed_events[i].instrument_token, 3045u);
        ASSERT_EQ(consumed_events[i].last_price, 83000 + static_cast<int64_t>(i % 50));
    }
}

TEST_CASE(MarketData_BackpressureDropCounting) {
    MarketDataPipeline pipeline;
    const size_t capacity = MarketDataPipeline::DEFAULT_QUEUE_CAPACITY;
    size_t pushed = 0;
    size_t dropped = 0;

    for (size_t i = 0; i < capacity + 500; ++i) {
        MarketEvent ev{};
        ev.sequence_number = i + 1;
        if (pipeline.enqueue_event(ev)) {
            ++pushed;
        } else {
            ++dropped;
        }
    }

    ASSERT_EQ(pushed, capacity);
    ASSERT_EQ(dropped, 500);
    ASSERT_EQ(pipeline.total_enqueued(), capacity);
    ASSERT_EQ(pipeline.total_dropped(), 500);
}

// ============================================================================
// 4. Binary Market Log (.mktlog) Recording & Replay
// ============================================================================

TEST_CASE(MarketLog_RecordingAndReplayRoundTrip) {
    const std::string test_file = "test_market_roundtrip.mktlog";
    const size_t event_count = 5000;
    auto original_events = generate_deterministic_market_events(event_count);

    // 1. Record
    {
        MarketEventRecorder recorder;
        ASSERT_TRUE(recorder.open(test_file));
        for (const auto& ev : original_events) {
            ASSERT_TRUE(recorder.write(ev));
        }
        recorder.close();
        ASSERT_EQ(recorder.events_written(), event_count);
    }

    uintmax_t file_size = std::filesystem::file_size(test_file);
    ASSERT_EQ(file_size, 64 + event_count * 128);

    // 2. Replay
    {
        MarketEventReplayer replayer;
        std::string err;
        ASSERT_TRUE(replayer.open(test_file, &err));
        ASSERT_EQ(replayer.header().magic, MKT_LOG_MAGIC);
        ASSERT_EQ(replayer.header().version, 1);
        ASSERT_EQ(replayer.header().record_size, 128u);
        ASSERT_EQ(replayer.header().header_size, 64u);
        ASSERT_EQ(replayer.header().event_count, event_count);

        ASSERT_TRUE(replayer.validate_full_checksum(&err));

        std::vector<MarketEvent> replayed_events;
        replayed_events.reserve(event_count);
        MarketEvent ev{};

        while (replayer.next(ev)) {
            replayed_events.push_back(ev);
        }

        ASSERT_EQ(replayed_events.size(), event_count);
        for (size_t i = 0; i < event_count; ++i) {
            ASSERT_TRUE(original_events[i] == replayed_events[i]);
        }

        replayer.close();
    }

    std::filesystem::remove(test_file);
}

TEST_CASE(MarketLog_CorruptedPayloadRejection) {
    const std::string test_file = "test_corrupt_market.mktlog";
    const size_t event_count = 50;

    {
        MarketEventRecorder recorder;
        recorder.open(test_file);
        for (size_t i = 0; i < event_count; ++i) {
            MarketEvent ev{};
            ev.instrument_token = 3045;
            ev.sequence_number = i + 1;
            ev.last_price = 83000;
            recorder.write(ev);
        }
        recorder.close();
    }

    FILE* f = std::fopen(test_file.c_str(), "r+b");
    ASSERT_TRUE(f != nullptr);
    std::fseek(f, 100, SEEK_SET);
    uint8_t val = 0;
    std::fread(&val, 1, 1, f);
    val ^= 0xFF;
    std::fseek(f, 100, SEEK_SET);
    std::fwrite(&val, 1, 1, f);
    std::fclose(f);

    MarketEventReplayer replayer;
    std::string err;
    ASSERT_TRUE(replayer.open(test_file, &err));
    ASSERT_FALSE(replayer.validate_full_checksum(&err));
    ASSERT_TRUE(err.find("Payload CRC32 mismatch") != std::string::npos);

    replayer.close();
    std::filesystem::remove(test_file);
}

// ============================================================================
// 5. Binary Log Format & Integrity Tests
// ============================================================================

TEST_CASE(MarketLog_RoundTrip_Small) {
    const std::string test_file = "test_market_small.mktlog";
    auto original_events = generate_deterministic_market_events(100);

    {
        MarketEventRecorder recorder;
        ASSERT_TRUE(recorder.open(test_file));
        for (const auto& ev : original_events) {
            ASSERT_TRUE(recorder.write(ev));
        }
        recorder.close();
        ASSERT_EQ(recorder.events_written(), 100ULL);
    }

    {
        MarketEventReplayer replayer;
        std::string err;
        ASSERT_TRUE(replayer.open(test_file, &err));
        ASSERT_EQ(replayer.header().event_count, 100ULL);

        std::vector<MarketEvent> replayed_events;
        MarketEvent ev{};
        while (replayer.next(ev)) {
            replayed_events.push_back(ev);
        }

        ASSERT_EQ(replayed_events.size(), original_events.size());
        for (size_t i = 0; i < original_events.size(); ++i) {
            ASSERT_TRUE(original_events[i] == replayed_events[i]);
        }
        ASSERT_TRUE(replayer.validate_full_checksum(&err));
        replayer.close();
    }

    std::filesystem::remove(test_file);
}

TEST_CASE(MarketLog_RoundTrip_Large) {
    const std::string test_file = "test_market_large.mktlog";
    constexpr size_t Count = 50000;
    auto original_events = generate_deterministic_market_events(Count);

    {
        MarketEventRecorder recorder;
        ASSERT_TRUE(recorder.open(test_file));
        for (const auto& ev : original_events) {
            ASSERT_TRUE(recorder.write(ev));
        }
        recorder.close();
        ASSERT_EQ(recorder.events_written(), Count);
    }

    {
        MarketEventReplayer replayer;
        std::string err;
        ASSERT_TRUE(replayer.open(test_file, &err));
        ASSERT_EQ(replayer.header().event_count, Count);

        size_t idx = 0;
        MarketEvent ev{};
        while (replayer.next(ev)) {
            ASSERT_TRUE(original_events[idx] == ev);
            ++idx;
        }

        ASSERT_EQ(idx, Count);
        ASSERT_TRUE(replayer.validate_full_checksum(&err));
        replayer.close();
    }

    std::filesystem::remove(test_file);
}

TEST_CASE(MarketLog_Rejection_InvalidMagic) {
    const std::string test_file = "test_market_corrupt_magic.mktlog";
    auto events = generate_deterministic_market_events(10);

    {
        MarketEventRecorder recorder;
        recorder.open(test_file);
        for (const auto& ev : events) recorder.write(ev);
        recorder.close();
    }

    FILE* f = std::fopen(test_file.c_str(), "r+b");
    ASSERT_TRUE(f != nullptr);
    uint32_t bad_magic = 0xDEADBEEF;
    std::fwrite(&bad_magic, sizeof(uint32_t), 1, f);
    std::fclose(f);

    MarketEventReplayer replayer;
    std::string err;
    ASSERT_FALSE(replayer.open(test_file, &err));
    ASSERT_TRUE(err.find("Invalid magic") != std::string::npos);

    std::filesystem::remove(test_file);
}

TEST_CASE(MarketLog_Rejection_UnsupportedVersion) {
    const std::string test_file = "test_market_corrupt_version.mktlog";
    auto events = generate_deterministic_market_events(10);

    {
        MarketEventRecorder recorder;
        recorder.open(test_file);
        for (const auto& ev : events) recorder.write(ev);
        recorder.close();
    }

    FILE* f = std::fopen(test_file.c_str(), "r+b");
    ASSERT_TRUE(f != nullptr);
    std::fseek(f, 4, SEEK_SET);
    uint16_t bad_version = 99;
    std::fwrite(&bad_version, sizeof(uint16_t), 1, f);
    std::fclose(f);

    MarketEventReplayer replayer;
    std::string err;
    ASSERT_FALSE(replayer.open(test_file, &err));
    ASSERT_TRUE(err.find("Unsupported") != std::string::npos || err.find("CRC") != std::string::npos);

    std::filesystem::remove(test_file);
}

TEST_CASE(MarketLog_Rejection_TruncatedHeader) {
    const std::string test_file = "test_market_truncated_hdr.mktlog";

    FILE* f = std::fopen(test_file.c_str(), "wb");
    ASSERT_TRUE(f != nullptr);
    uint8_t garbage[24] = {0};
    std::fwrite(garbage, 1, sizeof(garbage), f);
    std::fclose(f);

    MarketEventReplayer replayer;
    std::string err;
    ASSERT_FALSE(replayer.open(test_file, &err));
    ASSERT_TRUE(err.find("Truncated") != std::string::npos || err.find("small") != std::string::npos);

    std::filesystem::remove(test_file);
}

TEST_CASE(MarketLog_Rejection_TruncatedPayload) {
    const std::string test_file = "test_market_truncated_payload.mktlog";
    auto events = generate_deterministic_market_events(10);

    {
        MarketEventRecorder recorder;
        recorder.open(test_file);
        for (const auto& ev : events) recorder.write(ev);
        recorder.close();
    }

    // Truncate file
    std::filesystem::resize_file(test_file, 200);

    MarketEventReplayer replayer;
    std::string err;
    // Header check or checksum check must detect truncated payload
    if (replayer.open(test_file, &err)) {
        ASSERT_FALSE(replayer.validate_full_checksum(&err));
        replayer.close();
    } else {
        ASSERT_TRUE(true);
    }

    std::filesystem::remove(test_file);
}

TEST_CASE(MarketLog_Rejection_CorruptedPayloadChecksum) {
    const std::string test_file = "test_market_corrupt_data.mktlog";
    auto events = generate_deterministic_market_events(20);

    {
        MarketEventRecorder recorder;
        recorder.open(test_file);
        for (const auto& ev : events) recorder.write(ev);
        recorder.close();
    }

    FILE* f = std::fopen(test_file.c_str(), "r+b");
    ASSERT_TRUE(f != nullptr);
    std::fseek(f, 100, SEEK_SET);
    uint8_t byte_val = 0;
    std::fread(&byte_val, 1, 1, f);
    byte_val ^= 0xFF;
    std::fseek(f, 100, SEEK_SET);
    std::fwrite(&byte_val, 1, 1, f);
    std::fclose(f);

    MarketEventReplayer replayer;
    std::string err;
    ASSERT_TRUE(replayer.open(test_file, &err));
    ASSERT_FALSE(replayer.validate_full_checksum(&err));
    ASSERT_TRUE(err.find("Payload CRC32 mismatch") != std::string::npos);
    replayer.close();

    std::filesystem::remove(test_file);
}

TEST_CASE(MarketLog_EndToEnd_DeterministicReplay) {
    const std::string test_file = "test_market_e2e_replay.mktlog";
    const size_t event_count = 1000;
    auto events = generate_deterministic_market_events(event_count, 0x12345678ULL);

    {
        MarketEventRecorder recorder;
        ASSERT_TRUE(recorder.open(test_file));
        for (const auto& ev : events) {
            ASSERT_TRUE(recorder.write(ev));
        }
        recorder.close();
    }

    {
        MarketEventReplayer replayer;
        std::string err;
        ASSERT_TRUE(replayer.open(test_file, &err));
        ASSERT_TRUE(replayer.validate_full_checksum(&err));

        std::vector<MarketEvent> replayed;
        replayed.reserve(event_count);
        MarketEvent ev{};
        while (replayer.next(ev)) {
            replayed.push_back(ev);
        }
        replayer.close();

        ASSERT_EQ(replayed.size(), event_count);
        for (size_t i = 0; i < event_count; ++i) {
            ASSERT_TRUE(events[i] == replayed[i]);
        }
    }

    std::filesystem::remove(test_file);
}

TEST_CASE(MarketLog_ReplayThrough_SpscPipeline) {
    const std::string test_file = "test_market_spsc_replay.mktlog";
    const size_t event_count = 1000;
    auto events = generate_deterministic_market_events(event_count, 0xCAFEBABEULL);

    {
        MarketEventRecorder recorder;
        recorder.open(test_file);
        for (const auto& ev : events) recorder.write(ev);
        recorder.close();
    }

    MarketDataPipeline pipeline;
    std::vector<MarketEvent> pipeline_events;
    pipeline_events.reserve(event_count);

    pipeline.set_event_listener([&pipeline_events](const MarketEvent& ev) {
        pipeline_events.push_back(ev);
    });

    pipeline.start();

    {
        MarketEventReplayer replayer;
        std::string err;
        ASSERT_TRUE(replayer.open(test_file, &err));
        MarketEvent ev{};
        while (replayer.next(ev)) {
            pipeline.enqueue_event_wait(ev);
        }
        replayer.close();
    }

    pipeline.stop_and_join();

    ASSERT_EQ(pipeline_events.size(), event_count);
    ASSERT_EQ(pipeline.total_consumed(), event_count);
    for (size_t i = 0; i < event_count; ++i) {
        ASSERT_TRUE(events[i] == pipeline_events[i]);
    }

    std::filesystem::remove(test_file);
}
