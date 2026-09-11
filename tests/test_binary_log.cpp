#define _CRT_SECURE_NO_WARNINGS
#include "test_framework.hpp"
#include "hft/event.hpp"
#include "hft/binary_format.hpp"
#include "hft/event_recorder.hpp"
#include "hft/event_replayer.hpp"
#include "hft/matching_engine.hpp"
#include "hft/event_pipeline.hpp"

#include <vector>
#include <cstdio>
#include <filesystem>

using namespace hft;

namespace {

std::vector<OrderEvent> generate_deterministic_events(size_t count, uint64_t seed = 0x12345678ULL) {
    std::vector<OrderEvent> events;
    events.reserve(count);
    uint64_t state = seed;
    auto next_u64 = [&state]() -> uint64_t {
        state = state * 6364136223846793005ULL + 1ULL;
        return state >> 32;
    };
    auto next_range = [&](uint64_t min_v, uint64_t max_v) -> uint64_t {
        return min_v + (next_u64() % (max_v - min_v + 1));
    };

    OrderId next_id = 1;
    std::vector<OrderId> active_ids;

    for (size_t i = 0; i < count; ++i) {
        const uint64_t roll = next_range(1, 100);
        if (roll <= 60 || active_ids.empty()) {
            // Add
            OrderId id = next_id++;
            Side side = (next_u64() % 2 == 0) ? Side::Buy : Side::Sell;
            Price price = static_cast<Price>(next_range(9950, 10050));
            Quantity qty = static_cast<Quantity>(next_range(10, 100));
            events.push_back(OrderEvent::make_add(id, side, price, qty));
            active_ids.push_back(id);
        } else if (roll <= 80) {
            // Cancel
            size_t idx = static_cast<size_t>(next_range(0, active_ids.size() - 1));
            OrderId id = active_ids[idx];
            active_ids[idx] = active_ids.back();
            active_ids.pop_back();
            events.push_back(OrderEvent::make_cancel(id));
        } else {
            // Modify
            size_t idx = static_cast<size_t>(next_range(0, active_ids.size() - 1));
            OrderId id = active_ids[idx];
            Price new_price = static_cast<Price>(next_range(9960, 10040));
            Quantity new_qty = static_cast<Quantity>(next_range(10, 80));
            events.push_back(OrderEvent::make_modify(id, new_price, new_qty));
        }
    }
    return events;
}

} // namespace

// ============================================================================
// 1. Round-Trip Event Fidelity Tests
// ============================================================================

TEST_CASE(BinaryLog_RoundTrip_Small) {
    const std::string test_file = "test_small.hftlog";
    auto original_events = generate_deterministic_events(100);

    // Record
    {
        EventRecorder recorder;
        ASSERT_TRUE(recorder.open(test_file));
        for (const auto& ev : original_events) {
            ASSERT_TRUE(recorder.write(ev));
        }
        recorder.close();
        ASSERT_EQ(recorder.events_written(), 100ULL);
    }

    // Replay
    {
        EventReplayer replayer;
        std::string err;
        ASSERT_TRUE(replayer.open(test_file, &err));
        ASSERT_EQ(replayer.header().event_count, 100ULL);

        std::vector<OrderEvent> replayed_events;
        OrderEvent ev{};
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

TEST_CASE(BinaryLog_RoundTrip_Large) {
    const std::string test_file = "test_large.hftlog";
    constexpr size_t Count = 50000;
    auto original_events = generate_deterministic_events(Count);

    // Record
    {
        EventRecorder recorder;
        ASSERT_TRUE(recorder.open(test_file));
        for (const auto& ev : original_events) {
            ASSERT_TRUE(recorder.write(ev));
        }
        recorder.close();
        ASSERT_EQ(recorder.events_written(), Count);
    }

    // Replay
    {
        EventReplayer replayer;
        std::string err;
        ASSERT_TRUE(replayer.open(test_file, &err));
        ASSERT_EQ(replayer.header().event_count, Count);

        size_t idx = 0;
        OrderEvent ev{};
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

// ============================================================================
// 2. Failure & Corruption Rejection Tests
// ============================================================================

TEST_CASE(BinaryLog_Rejection_InvalidMagic) {
    const std::string test_file = "test_corrupt_magic.hftlog";
    auto events = generate_deterministic_events(10);

    {
        EventRecorder recorder;
        recorder.open(test_file);
        for (const auto& ev : events) recorder.write(ev);
        recorder.close();
    }

    // Corrupt magic bytes at offset 0
    FILE* f = std::fopen(test_file.c_str(), "r+b");
    ASSERT_TRUE(f != nullptr);
    uint32_t bad_magic = 0xDEADBEEF;
    std::fwrite(&bad_magic, sizeof(uint32_t), 1, f);
    std::fclose(f);

    EventReplayer replayer;
    std::string err;
    ASSERT_FALSE(replayer.open(test_file, &err));
    ASSERT_TRUE(err.find("Invalid magic number") != std::string::npos);

    std::filesystem::remove(test_file);
}

TEST_CASE(BinaryLog_Rejection_UnsupportedVersion) {
    const std::string test_file = "test_corrupt_version.hftlog";
    auto events = generate_deterministic_events(10);

    {
        EventRecorder recorder;
        recorder.open(test_file);
        for (const auto& ev : events) recorder.write(ev);
        recorder.close();
    }

    // Corrupt version at offset 4
    FILE* f = std::fopen(test_file.c_str(), "r+b");
    ASSERT_TRUE(f != nullptr);
    std::fseek(f, 4, SEEK_SET);
    uint16_t bad_version = 99;
    std::fwrite(&bad_version, sizeof(uint16_t), 1, f);
    std::fclose(f);

    EventReplayer replayer;
    std::string err;
    ASSERT_FALSE(replayer.open(test_file, &err));

    std::filesystem::remove(test_file);
}

TEST_CASE(BinaryLog_Rejection_TruncatedHeader) {
    const std::string test_file = "test_truncated_hdr.hftlog";

    // Write file smaller than 64 bytes
    FILE* f = std::fopen(test_file.c_str(), "wb");
    ASSERT_TRUE(f != nullptr);
    uint8_t garbage[24] = {0};
    std::fwrite(garbage, 1, sizeof(garbage), f);
    std::fclose(f);

    EventReplayer replayer;
    std::string err;
    ASSERT_FALSE(replayer.open(test_file, &err));
    ASSERT_TRUE(err.find("File too small") != std::string::npos);

    std::filesystem::remove(test_file);
}

TEST_CASE(BinaryLog_Rejection_TruncatedPayload) {
    const std::string test_file = "test_truncated_payload.hftlog";
    auto events = generate_deterministic_events(10); // 10 * 32 = 320 bytes payload + 64B header = 384B

    {
        EventRecorder recorder;
        recorder.open(test_file);
        for (const auto& ev : events) recorder.write(ev);
        recorder.close();
    }

    // Truncate file from 384 bytes to 200 bytes
    std::filesystem::resize_file(test_file, 200);

    EventReplayer replayer;
    std::string err;
    ASSERT_FALSE(replayer.open(test_file, &err));
    ASSERT_TRUE(err.find("File size mismatch") != std::string::npos);

    std::filesystem::remove(test_file);
}

TEST_CASE(BinaryLog_Rejection_CorruptedPayloadChecksum) {
    const std::string test_file = "test_corrupt_data.hftlog";
    auto events = generate_deterministic_events(20);

    {
        EventRecorder recorder;
        recorder.open(test_file);
        for (const auto& ev : events) recorder.write(ev);
        recorder.close();
    }

    // Flip 1 byte in payload (at offset 100)
    FILE* f = std::fopen(test_file.c_str(), "r+b");
    ASSERT_TRUE(f != nullptr);
    std::fseek(f, 100, SEEK_SET);
    uint8_t byte_val = 0;
    std::fread(&byte_val, 1, 1, f);
    byte_val ^= 0xFF;
    std::fseek(f, 100, SEEK_SET);
    std::fwrite(&byte_val, 1, 1, f);
    std::fclose(f);

    EventReplayer replayer;
    std::string err;
    ASSERT_TRUE(replayer.open(test_file, &err)); // Header is valid
    ASSERT_FALSE(replayer.validate_full_checksum(&err)); // CRC32 must detect corruption
    ASSERT_TRUE(err.find("Payload CRC32 mismatch") != std::string::npos);
    replayer.close();

    std::filesystem::remove(test_file);
}

// ============================================================================
// 3. End-to-End Deterministic Replay (Reference Engine vs Replayed Engine)
// ============================================================================

TEST_CASE(BinaryLog_EndToEnd_DeterministicReplay) {
    const std::string test_file = "test_e2e_replay.hftlog";
    const size_t event_count = 1000;
    auto events = generate_deterministic_events(event_count, 0x12345678ULL);

    // 1. Run Direct Reference Execution
    MatchingEngine direct_engine;
    direct_engine.reserve(event_count);
    std::vector<Trade> direct_trades;
    std::vector<Trade> step_trades;
    std::vector<OrderResult> direct_results;

    for (const auto& ev : events) {
        step_trades.clear();
        OrderResult res = OrderResult::RejectedUnchanged;
        switch (ev.type) {
            case EventType::Add:
                res = direct_engine.submit_limit_order(ev.id, ev.side, ev.price, ev.qty, step_trades);
                break;
            case EventType::Cancel:
                res = direct_engine.cancel_order(ev.id);
                break;
            case EventType::Modify:
                res = direct_engine.modify_order(ev.id, ev.price, ev.qty, step_trades);
                break;
        }
        direct_results.push_back(res);
        for (const auto& t : step_trades) direct_trades.push_back(t);
    }

    // 2. Record to .hftlog
    {
        EventRecorder recorder;
        ASSERT_TRUE(recorder.open(test_file));
        for (const auto& ev : events) {
            ASSERT_TRUE(recorder.write(ev));
        }
        recorder.close();
    }

    // 3. Replay from .hftlog into Replay Engine
    MatchingEngine replay_engine;
    replay_engine.reserve(event_count);
    std::vector<Trade> replay_trades;
    std::vector<OrderResult> replay_results;

    {
        EventReplayer replayer;
        std::string err;
        ASSERT_TRUE(replayer.open(test_file, &err));

        OrderEvent ev{};
        while (replayer.next(ev)) {
            step_trades.clear();
            OrderResult res = OrderResult::RejectedUnchanged;
            switch (ev.type) {
                case EventType::Add:
                    res = replay_engine.submit_limit_order(ev.id, ev.side, ev.price, ev.qty, step_trades);
                    break;
                case EventType::Cancel:
                    res = replay_engine.cancel_order(ev.id);
                    break;
                case EventType::Modify:
                    res = replay_engine.modify_order(ev.id, ev.price, ev.qty, step_trades);
                    break;
            }
            replay_results.push_back(res);
            for (const auto& t : step_trades) replay_trades.push_back(t);
        }
        replayer.close();
    }

    // 4. Assert Bit-Exact Equivalence
    ASSERT_EQ(direct_results.size(), replay_results.size());
    for (size_t i = 0; i < direct_results.size(); ++i) {
        ASSERT_EQ(direct_results[i], replay_results[i]);
    }

    ASSERT_EQ(direct_trades.size(), replay_trades.size());
    for (size_t i = 0; i < direct_trades.size(); ++i) {
        ASSERT_TRUE(direct_trades[i] == replay_trades[i]);
    }

    const auto& db = direct_engine.book();
    const auto& rb = replay_engine.book();

    ASSERT_EQ(db.best_bid(), rb.best_bid());
    ASSERT_EQ(db.best_ask(), rb.best_ask());
    ASSERT_EQ(db.best_bid_qty(), rb.best_bid_qty());
    ASSERT_EQ(db.best_ask_qty(), rb.best_ask_qty());
    ASSERT_EQ(db.bid_depth(), rb.bid_depth());
    ASSERT_EQ(db.ask_depth(), rb.ask_depth());
    ASSERT_EQ(db.total_orders(), rb.total_orders());
    ASSERT_EQ(db.total_bid_qty(), rb.total_bid_qty());
    ASSERT_EQ(db.total_ask_qty(), rb.total_ask_qty());

    std::string d_err, r_err;
    bool d_ok = direct_engine.verify_invariants(&d_err);
    if (!d_ok) std::cerr << "Direct Engine Invariant Error: " << d_err << "\n";
    ASSERT_TRUE(d_ok);

    bool r_ok = replay_engine.verify_invariants(&r_err);
    if (!r_ok) std::cerr << "Replay Engine Invariant Error: " << r_err << "\n";
    ASSERT_TRUE(r_ok);

    std::filesystem::remove(test_file);
}

// ============================================================================
// 4. Replay Through Threaded SPSC Pipeline
// ============================================================================

TEST_CASE(BinaryLog_ReplayThrough_SpscPipeline) {
    const std::string test_file = "test_spsc_replay.hftlog";
    const size_t event_count = 1000;
    auto events = generate_deterministic_events(event_count, 0xCAFEBABEULL);

    // Record events to binary log
    {
        EventRecorder recorder;
        recorder.open(test_file);
        for (const auto& ev : events) recorder.write(ev);
        recorder.close();
    }

    // Replay through SPSC Pipeline (Replayer acts as producer)
    MatchingEnginePipeline pipeline(MatchingEnginePipeline::DEFAULT_QUEUE_CAPACITY, event_count);
    pipeline.start();

    {
        EventReplayer replayer;
        replayer.open(test_file);
        OrderEvent ev{};
        while (replayer.next(ev)) {
            pipeline.enqueue_event_wait(ev);
        }
        replayer.close();
    }

    pipeline.stop_and_join();

    // Verify all events processed
    ASSERT_EQ(pipeline.events_processed(), event_count);
    ASSERT_EQ(pipeline.results().size(), event_count);

    std::string pipe_err;
    bool p_ok = pipeline.engine().verify_invariants(&pipe_err);
    if (!p_ok) std::cerr << "Pipeline Engine Invariant Error: " << pipe_err << "\n";
    ASSERT_TRUE(p_ok);

    std::filesystem::remove(test_file);
}
