#define _CRT_SECURE_NO_WARNINGS
#include "test_framework.hpp"

#include "hft/market_event.hpp"
#include "hft/broker/angel_types.hpp"
#include "hft/broker/angel_decoder.hpp"
#include "hft/broker/mock_angel_feed.hpp"
#include "hft/market_data_pipeline.hpp"
#include "hft/market_format.hpp"
#include "hft/market_recorder.hpp"
#include "hft/market_replayer.hpp"

#include <vector>
#include <cstdio>
#include <filesystem>
#include <chrono>

using namespace hft;
using namespace hft::broker;

// ============================================================================
// 1. MarketEvent Layout & Properties
// ============================================================================

TEST_CASE(MarketEvent_LayoutAndAlignment) {
    ASSERT_EQ(sizeof(MarketEvent), 128);
    ASSERT_EQ(alignof(MarketEvent), 64);
    ASSERT_TRUE(std::is_trivially_copyable_v<MarketEvent>);
    ASSERT_TRUE(std::is_standard_layout_v<MarketEvent>);
}

// ============================================================================
// 2. Decoder Tests (LTP Mode 1, Quote Mode 2, SnapQuote Mode 3)
// ============================================================================

TEST_CASE(AngelDecoder_LtpMode51Bytes) {
    uint8_t buffer[64]{0};
    size_t len = MockAngelFeed::build_ltp_packet(buffer, sizeof(buffer), "3045", 83050, 101, 1710000000123LL, AngelConstants::EXCH_NSE_CM);
    ASSERT_EQ(len, 51);

    MarketEvent ev{};
    uint64_t recv_ts = 987654321ULL;
    bool ok = AngelDecoder::decode(buffer, len, ev, recv_ts);

    ASSERT_TRUE(ok);
    ASSERT_EQ(ev.instrument_token, 3045u);
    ASSERT_EQ(ev.exchange_type, AngelConstants::EXCH_NSE_CM);
    ASSERT_EQ(ev.subscription_mode, AngelConstants::MODE_LTP);
    ASSERT_EQ(ev.sequence_number, 101);
    ASSERT_EQ(ev.exchange_timestamp, 1710000000123000000ULL); // ms -> ns
    ASSERT_EQ(ev.receive_timestamp, recv_ts);
    ASSERT_EQ(ev.last_price, 83050); // 830.50 in paise
    ASSERT_EQ(ev.last_quantity, 0);
    ASSERT_EQ(ev.best_bid_price, 0);
    ASSERT_EQ(ev.best_ask_price, 0);
}

TEST_CASE(AngelDecoder_QuoteMode147Bytes) {
    uint8_t buffer[200]{0};
    size_t len = MockAngelFeed::build_quote_packet(
        buffer, sizeof(buffer), "26009", 2245000, 50, 502, 1710000000500LL, 150000,
        2240000, 2250000, 2235000, 2242000, AngelConstants::EXCH_NSE_CM);
    ASSERT_EQ(len, 147);

    MarketEvent ev{};
    uint64_t recv_ts = 11223344ULL;
    bool ok = AngelDecoder::decode(buffer, len, ev, recv_ts);

    ASSERT_TRUE(ok);
    ASSERT_EQ(ev.instrument_token, 26009u);
    ASSERT_EQ(ev.subscription_mode, AngelConstants::MODE_QUOTE);
    ASSERT_EQ(ev.sequence_number, 502);
    ASSERT_EQ(ev.exchange_timestamp, 1710000000500000000ULL);
    ASSERT_EQ(ev.receive_timestamp, recv_ts);
    ASSERT_EQ(ev.last_price, 2245000);
    ASSERT_EQ(ev.last_quantity, 50);
    ASSERT_EQ(ev.volume, 150000);
    ASSERT_EQ(ev.best_bid_price, 0);
}

TEST_CASE(AngelDecoder_SnapQuoteMode347Bytes) {
    uint8_t buffer[400]{0};
    size_t len = MockAngelFeed::build_snap_quote_packet(
        buffer, sizeof(buffer), "3045", 83050, 100, 83045, 500, 83055, 600, 999, 1710000000999LL, 75000);
    ASSERT_EQ(len, 347);

    MarketEvent ev{};
    uint64_t recv_ts = 55667788ULL;
    bool ok = AngelDecoder::decode(buffer, len, ev, recv_ts);

    ASSERT_TRUE(ok);
    ASSERT_EQ(ev.instrument_token, 3045u);
    ASSERT_EQ(ev.subscription_mode, AngelConstants::MODE_SNAP_QUOTE);
    ASSERT_EQ(ev.last_price, 83050);
    ASSERT_EQ(ev.last_quantity, 100);
    ASSERT_EQ(ev.best_bid_price, 83045);
    ASSERT_EQ(ev.best_bid_quantity, 500);
    ASSERT_EQ(ev.best_ask_price, 83055);
    ASSERT_EQ(ev.best_ask_quantity, 600);
    ASSERT_EQ(ev.volume, 75000);
}

// ============================================================================
// 3. Token Parsing & Validation
// ============================================================================

TEST_CASE(AngelDecoder_TokenParsing) {
    ASSERT_EQ(AngelDecoder::parse_token("3045"), 3045u);
    ASSERT_EQ(AngelDecoder::parse_token("26009"), 26009u);
    ASSERT_EQ(AngelDecoder::parse_token("0"), 0u);
    ASSERT_EQ(AngelDecoder::parse_token("11536\0trailing"), 11536u);
    ASSERT_EQ(AngelDecoder::parse_token("INVALID"), 0u);
}

TEST_CASE(AngelDecoder_PriceAndQuantityConversion) {
    uint8_t buffer[64]{0};
    MockAngelFeed::build_ltp_packet(buffer, sizeof(buffer), "100", 12345678, 1, 1000);

    MarketEvent ev{};
    ASSERT_TRUE(AngelDecoder::decode(buffer, 51, ev, 123));
    ASSERT_EQ(ev.last_price, 12345678); // 123456.78 in paise
}

TEST_CASE(AngelDecoder_TimestampConversion) {
    uint8_t buffer[64]{0};
    int64_t test_ms = 1672531199000LL; // 2022-12-31 23:59:59 UTC
    MockAngelFeed::build_ltp_packet(buffer, sizeof(buffer), "1", 100, 1, test_ms);

    MarketEvent ev{};
    uint64_t local_now = 999888777ULL;
    ASSERT_TRUE(AngelDecoder::decode(buffer, 51, ev, local_now));
    ASSERT_EQ(ev.exchange_timestamp, static_cast<uint64_t>(test_ms) * 1000000ULL);
    ASSERT_EQ(ev.receive_timestamp, local_now);
}

TEST_CASE(AngelDecoder_MalformedPacketRejection) {
    uint8_t buffer[200]{0};
    MarketEvent ev{};

    // Null buffer
    ASSERT_FALSE(AngelDecoder::decode(nullptr, 51, ev, 0));

    // Length too small for any mode (< 51)
    ASSERT_FALSE(AngelDecoder::decode(buffer, 50, ev, 0));

    // Mode is Quote (2) but length is only 51 (< 147)
    buffer[0] = AngelConstants::MODE_QUOTE;
    ASSERT_FALSE(AngelDecoder::decode(buffer, 51, ev, 0));

    // Mode is SnapQuote (3) but length is only 147 (< 347)
    buffer[0] = AngelConstants::MODE_SNAP_QUOTE;
    ASSERT_FALSE(AngelDecoder::decode(buffer, 147, ev, 0));

    // Invalid subscription mode (e.g. 99)
    buffer[0] = 99;
    ASSERT_FALSE(AngelDecoder::decode(buffer, 51, ev, 0));
}

// ============================================================================
// 4. SPSC Market Data Pipeline & Backpressure Tests
// ============================================================================

TEST_CASE(MarketData_SpscThreadTransfer) {
    MarketDataPipeline pipeline;
    std::vector<MarketEvent> consumed_events;
    consumed_events.reserve(5000);

    pipeline.set_event_listener([&consumed_events](const MarketEvent& ev) {
        consumed_events.push_back(ev);
    });

    pipeline.start();

    // Push 5,000 deterministic events
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

    // Verify ordering and field fidelity
    for (size_t i = 0; i < event_count; ++i) {
        ASSERT_EQ(consumed_events[i].sequence_number, i + 1);
        ASSERT_EQ(consumed_events[i].instrument_token, 3045u);
        ASSERT_EQ(consumed_events[i].last_price, 83000 + static_cast<int64_t>(i % 50));
    }
}

TEST_CASE(MarketData_BackpressureDropCounting) {
    // Test that when consumer is not draining, queue-full condition increments dropped_events
    MarketDataPipeline pipeline;
    // Do NOT start pipeline consumer thread

    const size_t capacity = MarketDataPipeline::DEFAULT_QUEUE_CAPACITY;
    size_t pushed = 0;
    size_t dropped = 0;

    // Push more events than capacity
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
    ASSERT_EQ(pipeline.total_queued(), capacity);
    ASSERT_EQ(pipeline.total_dropped(), 500);
    ASSERT_EQ(pipeline.total_normalized(), capacity + 500);
}

// ============================================================================
// 5. Binary Market Log (.mktlog) Recording & Replay Tests
// ============================================================================

TEST_CASE(MarketLog_RecordingAndReplayRoundTrip) {
    const std::string test_file = "test_market_roundtrip.mktlog";
    const size_t event_count = 5000;

    std::vector<MarketEvent> original_events;
    original_events.reserve(event_count);

    for (size_t i = 0; i < event_count; ++i) {
        MarketEvent ev{};
        ev.instrument_token = 3045u;
        ev.exchange_type = 1;
        ev.subscription_mode = 3;
        ev.sequence_number = i + 1;
        ev.exchange_timestamp = 1710000000000ULL + i * 1000000ULL;
        ev.receive_timestamp = 1000000000ULL + i * 50ULL;
        ev.last_price = 83000 + static_cast<int64_t>(i % 100);
        ev.last_quantity = 50 + (i % 20);
        ev.best_bid_price = ev.last_price - 5;
        ev.best_bid_quantity = 200;
        ev.best_ask_price = ev.last_price + 5;
        ev.best_ask_quantity = 300;
        ev.volume = 10000 + i * 10;
        original_events.push_back(ev);
    }

    // 1. Record to .mktlog
    {
        MarketEventRecorder recorder;
        ASSERT_TRUE(recorder.open(test_file));
        for (const auto& ev : original_events) {
            ASSERT_TRUE(recorder.write(ev));
        }
        recorder.close();
        ASSERT_EQ(recorder.events_written(), event_count);
    }

    // Check file size: 64B header + 5000 * 128B = 640,064 bytes
    uintmax_t file_size = std::filesystem::file_size(test_file);
    ASSERT_EQ(file_size, 64 + event_count * 128);

    // 2. Replay and verify exact field fidelity
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

    // Flip 1 byte in payload at offset 100
    FILE* f = std::fopen(test_file.c_str(), "r+b");
    ASSERT_TRUE(f != nullptr);
    std::fseek(f, 100, SEEK_SET);
    uint8_t val = 0;
    std::fread(&val, 1, 1, f);
    val ^= 0xFF;
    std::fseek(f, 100, SEEK_SET);
    std::fwrite(&val, 1, 1, f);
    std::fclose(f);

    // Replayer must open header successfully, but detect payload CRC mismatch
    MarketEventReplayer replayer;
    std::string err;
    ASSERT_TRUE(replayer.open(test_file, &err));
    ASSERT_FALSE(replayer.validate_full_checksum(&err));
    ASSERT_TRUE(err.find("Payload CRC32 mismatch") != std::string::npos);

    replayer.close();
    std::filesystem::remove(test_file);
}
