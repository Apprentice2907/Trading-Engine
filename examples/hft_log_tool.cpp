#include "hft/binary_format.hpp"
#include "hft/event_recorder.hpp"
#include "hft/event_replayer.hpp"
#include "hft/matching_engine.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace {

void print_usage(const char* prog_name) {
    std::cout << "HFT Binary Event Log Tool (.hftlog)\n";
    std::cout << "Usage:\n";
    std::cout << "  " << prog_name << " record <output.hftlog> [count]   Record synthetic event stream\n";
    std::cout << "  " << prog_name << " replay <input.hftlog>            Replay events through MatchingEngine\n";
    std::cout << "  " << prog_name << " verify <input.hftlog>            Validate file integrity and checksums\n";
}

int cmd_record(const std::string& path, uint64_t count) {
    std::cout << "Recording " << count << " synthetic events to " << path << "...\n";

    hft::EventRecorder recorder;
    if (!recorder.open(path)) {
        std::cerr << "Error: Failed to open " << path << " for writing.\n";
        return 1;
    }

    std::mt19937_64 rng(1337);
    std::uniform_int_distribution<int> op_dist(0, 99);
    std::uniform_int_distribution<int> side_dist(0, 1);
    std::uniform_int_distribution<int64_t> price_offset(-50, 50);
    std::uniform_int_distribution<uint32_t> qty_dist(1, 100);

    const int64_t mid_price = 10000;
    std::vector<hft::OrderId> live_orders;
    live_orders.reserve(100000);

    hft::OrderId next_id = 1;

    auto start_time = std::chrono::high_resolution_clock::now();

    for (uint64_t i = 0; i < count; ++i) {
        int op = op_dist(rng);

        if (op < 60 || live_orders.empty()) {
            // 60% New Limit Order
            hft::Side side = (side_dist(rng) == 0) ? hft::Side::Buy : hft::Side::Sell;
            int64_t price = mid_price + price_offset(rng);
            if (price <= 0) price = 1;
            uint32_t qty = qty_dist(rng);
            hft::OrderId id = next_id++;

            hft::OrderEvent ev = hft::OrderEvent::make_add(id, side, price, qty);
            if (!recorder.write(ev)) {
                std::cerr << "Error writing event " << i << "\n";
                return 1;
            }
            live_orders.push_back(id);
        } else if (op < 85) {
            // 25% Cancel
            size_t idx = rng() % live_orders.size();
            hft::OrderId id = live_orders[idx];
            live_orders[idx] = live_orders.back();
            live_orders.pop_back();

            hft::OrderEvent ev = hft::OrderEvent::make_cancel(id);
            if (!recorder.write(ev)) {
                std::cerr << "Error writing event " << i << "\n";
                return 1;
            }
        } else {
            // 15% Modify
            size_t idx = rng() % live_orders.size();
            hft::OrderId id = live_orders[idx];
            int64_t new_price = mid_price + price_offset(rng);
            if (new_price <= 0) new_price = 1;
            uint32_t new_qty = qty_dist(rng);

            hft::OrderEvent ev = hft::OrderEvent::make_modify(id, new_price, new_qty);
            if (!recorder.write(ev)) {
                std::cerr << "Error writing event " << i << "\n";
                return 1;
            }
        }
    }

    recorder.close();

    auto end_time = std::chrono::high_resolution_clock::now();
    double duration_sec = std::chrono::duration<double>(end_time - start_time).count();
    uintmax_t file_size = std::filesystem::file_size(path);

    std::cout << "Recording complete.\n";
    std::cout << "  Events written:  " << count << "\n";
    std::cout << "  File size:       " << file_size << " bytes ("
              << (file_size / (1024.0 * 1024.0)) << " MB)\n";
    std::cout << "  Elapsed time:    " << (duration_sec * 1000.0) << " ms\n";
    std::cout << "  Throughput:      " << static_cast<uint64_t>(count / duration_sec) << " events/sec ("
              << (file_size / (duration_sec * 1024.0 * 1024.0)) << " MB/sec)\n";

    return 0;
}

int cmd_replay(const std::string& path) {
    std::cout << "Replaying events from " << path << "...\n";

    hft::EventReplayer replayer;
    std::string err;
    if (!replayer.open(path, &err)) {
        std::cerr << "Error opening log file: " << err << "\n";
        return 1;
    }

    hft::MatchingEngine engine;
    engine.reserve(replayer.header().event_count);
    std::vector<hft::Trade> trades;
    trades.reserve(16);

    uint64_t events_replayed = 0;
    uint64_t trade_count = 0;
    uint64_t total_volume = 0;

    hft::OrderEvent ev;

    auto start_time = std::chrono::high_resolution_clock::now();

    while (replayer.next(ev)) {
        trades.clear();
        switch (ev.type) {
            case hft::EventType::Add:
                engine.submit_limit_order(ev.id, ev.side, ev.price, ev.qty, trades);
                break;
            case hft::EventType::Cancel:
                engine.cancel_order(ev.id);
                break;
            case hft::EventType::Modify:
                engine.modify_order(ev.id, ev.price, ev.qty, trades);
                break;
        }

        for (const auto& t : trades) {
            ++trade_count;
            total_volume += t.quantity;
        }
        ++events_replayed;
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    double duration_sec = std::chrono::duration<double>(end_time - start_time).count();

    if (events_replayed != replayer.header().event_count) {
        std::cerr << "Warning: Expected " << replayer.header().event_count
                  << " events, but replayed " << events_replayed << "\n";
    }

    std::cout << "Replay complete.\n";
    std::cout << "  Events replayed:   " << events_replayed << "\n";
    std::cout << "  Trades generated:  " << trade_count << " (engine reported: "
              << engine.total_trades_generated() << ")\n";
    std::cout << "  Total volume:      " << total_volume << "\n";
    std::cout << "  Active orders:     " << engine.book().total_orders() << "\n";

    auto best_bid = engine.book().best_bid();
    auto best_ask = engine.book().best_ask();
    std::cout << "  Best bid:          ";
    if (best_bid.has_value()) {
        std::cout << *best_bid << " (qty: " << engine.book().best_bid_qty() << ")\n";
    } else {
        std::cout << "None\n";
    }

    std::cout << "  Best ask:          ";
    if (best_ask.has_value()) {
        std::cout << *best_ask << " (qty: " << engine.book().best_ask_qty() << ")\n";
    } else {
        std::cout << "None\n";
    }

    std::cout << "  Elapsed time:      " << (duration_sec * 1000.0) << " ms\n";
    std::cout << "  Throughput:        " << static_cast<uint64_t>(events_replayed / duration_sec)
              << " events/sec\n";

    return 0;
}

int cmd_verify(const std::string& path) {
    std::cout << "Verifying integrity of " << path << "...\n";

    hft::EventReplayer replayer;
    std::string err;
    if (!replayer.open(path, &err)) {
        std::cerr << "[FAIL] Failed to open log file: " << err << "\n";
        return 1;
    }

    const auto& hdr = replayer.header();
    char magic_str[5] = {0};
    std::memcpy(magic_str, &hdr.magic, 4);

    std::cout << "Header Details:\n";
    std::cout << "  Magic:             0x" << std::hex << hdr.magic << std::dec
              << " ('" << magic_str << "')\n";
    std::cout << "  Version:           " << static_cast<uint32_t>(hdr.version) << "\n";
    std::cout << "  Header Size:       " << static_cast<uint32_t>(hdr.header_size) << " bytes\n";
    std::cout << "  Record Size:       " << static_cast<uint32_t>(hdr.record_size) << " bytes\n";
    std::cout << "  Event Count:       " << hdr.event_count << "\n";
    std::cout << "  Header CRC32:      0x" << std::hex << hdr.header_crc32 << std::dec << "\n";
    std::cout << "  Data CRC32:        0x" << std::hex << hdr.data_crc32 << std::dec << "\n";

    if (!replayer.validate_full_checksum(&err)) {
        std::cerr << "[FAIL] Integrity check failed: " << err << "\n";
        return 1;
    }

    std::cout << "[OK] File integrity successfully verified. Header and Data CRC32 checksums match perfectly.\n";
    return 0;
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc < 3) {
        print_usage(argv[0]);
        return 1;
    }

    std::string mode = argv[1];
    std::string file_path = argv[2];

    if (mode == "record") {
        uint64_t count = 100000;
        if (argc >= 4) {
            count = std::stoull(argv[3]);
        }
        return cmd_record(file_path, count);
    } else if (mode == "replay") {
        return cmd_replay(file_path);
    } else if (mode == "verify") {
        return cmd_verify(file_path);
    } else {
        std::cerr << "Unknown mode: " << mode << "\n\n";
        print_usage(argv[0]);
        return 1;
    }
}
