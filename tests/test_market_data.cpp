#define _CRT_SECURE_NO_WARNINGS
#include "test_framework.hpp"
#include "hft/market_data.hpp"

#include <vector>
#include <cstdio>
#include <filesystem>
#include <chrono>

using namespace hft;
using namespace hft::broker;

namespace {

std::vector<MarketEvent> generate_deterministic_market_events(size_t count, uint64_t seed = 0x12345678ULL) {
    std::vector<MarketEvent> events;
    events.reserve(count);
    uint64_t state = seed;
    auto next_u64 = [&state]() -> uint64_t {
        state = state * 6364136223846793005ULL + 1ULL;
        return state >> 32;
    };
    for (size_t i = 0; i < count; ++i) {
        MarketEvent ev{};
        ev.instrument_token = 3045;
        ev.exchange_type = 1;
        ev.subscription_mode = 3;
        ev.sequence_number = i + 1;
        ev.exchange_timestamp = 1710000000000ULL + i * 1000000ULL;
        ev.receive_timestamp = 1000000000ULL + i * 50ULL;
        ev.last_price = 83000 + static_cast<int64_t>(next_u64() % 100);
        ev.last_quantity = 50 + (next_u64() % 20);
        ev.best_bid_price = ev.last_price - 5;
        ev.best_bid_quantity = 200;
        ev.best_ask_price = ev.last_price + 5;
        ev.best_ask_quantity = 300;
        ev.volume = 10000 + i * 10;
        events.push_back(ev);
    }
    return events;
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
// 2. Broker Wire Protocol Decoders (LTP, Quote, SnapQuote)
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
    ASSERT_EQ(ev.exchange_timestamp, 1710000000123000000ULL);
    ASSERT_EQ(ev.receive_timestamp, recv_ts);
    ASSERT_EQ(ev.last_price, 83050);
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
    ASSERT_EQ(ev.last_price, 12345678);
}

TEST_CASE(AngelDecoder_TimestampConversion) {
    uint8_t buffer[64]{0};
    int64_t test_ms = 1672531199000LL;
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

    ASSERT_FALSE(AngelDecoder::decode(nullptr, 51, ev, 0));
    ASSERT_FALSE(AngelDecoder::decode(buffer, 50, ev, 0));

    buffer[0] = AngelConstants::MODE_QUOTE;
    ASSERT_FALSE(AngelDecoder::decode(buffer, 51, ev, 0));

    buffer[0] = AngelConstants::MODE_SNAP_QUOTE;
    ASSERT_FALSE(AngelDecoder::decode(buffer, 147, ev, 0));

    buffer[0] = 99;
    ASSERT_FALSE(AngelDecoder::decode(buffer, 51, ev, 0));
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
