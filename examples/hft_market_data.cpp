#include "hft/market_event.hpp"
#include "hft/broker/angel_types.hpp"
#include "hft/broker/angel_decoder.hpp"
#include "hft/broker/mock_angel_feed.hpp"
#include "hft/broker/angel_client.hpp"
#include "hft/market_data_pipeline.hpp"
#include "hft/market_recorder.hpp"
#include "hft/market_replayer.hpp"

#include <iostream>
#include <iomanip>
#include <string>
#include <chrono>
#include <thread>
#include <atomic>
#include <csignal>
#include <filesystem>

namespace {

std::atomic<bool> g_shutdown{false};

void signal_handler(int) {
    g_shutdown.store(true);
}

void print_usage(const char* prog) {
    std::cout << "HFT Angel One SmartAPI Market Data Tool\n";
    std::cout << "Usage:\n";
    std::cout << "  " << prog << " live [--mock] [--seconds N] [token]     Stream live or mock market data\n";
    std::cout << "  " << prog << " record <output.mktlog> [--mock] [count] Record market data to binary log\n";
    std::cout << "  " << prog << " replay <input.mktlog>                  Replay recorded market data offline\n";
}

int cmd_live(bool mock_mode, uint32_t token, uint32_t duration_sec = 0) {
    std::cout << "Starting market data stream (" << (mock_mode ? "MOCK FEED" : "LIVE ANGEL ONE") << ")...\n";
    if (duration_sec > 0) {
        std::cout << "Running for controlled duration: " << duration_sec << " seconds...\n";
    }

    hft::MarketDataPipeline pipeline;
    pipeline.start();

    std::signal(SIGINT, signal_handler);

    auto stream_start = std::chrono::steady_clock::now();

    if (mock_mode) {
        std::cout << "Streaming deterministic mock ticks for token " << token << " (Ctrl+C to stop)...\n";
        auto packets = hft::broker::MockAngelFeed::generate_synthetic_stream(1000000);
        size_t idx = 0;

        auto last_report = std::chrono::steady_clock::now();
        uint64_t last_consumed = 0;

        while (!g_shutdown.load() && idx < packets.size()) {
            if (duration_sec > 0) {
                auto curr = std::chrono::steady_clock::now();
                if (std::chrono::duration<double>(curr - stream_start).count() >= duration_sec) {
                    break;
                }
            }
            hft::MarketEvent ev{};
            uint64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();

            if (hft::broker::AngelDecoder::decode(packets[idx].data(), packets[idx].size(), ev, now_ns)) {
                pipeline.enqueue_event(ev);
            }
            ++idx;

            std::this_thread::sleep_for(std::chrono::microseconds(100));

            auto now = std::chrono::steady_clock::now();
            double elapsed = std::chrono::duration<double>(now - last_report).count();
            if (elapsed >= 1.0) {
                uint64_t current_consumed = pipeline.total_consumed();
                uint64_t rate = static_cast<uint64_t>((current_consumed - last_consumed) / elapsed);
                last_consumed = current_consumed;
                last_report = now;

                auto latest = pipeline.latest_event();
                if (latest) {
                    std::cout << "\r[FEED] Token: " << latest->instrument_token
                              << " | Events: " << current_consumed
                              << " (" << rate << " ev/s) | Drops: " << pipeline.total_dropped()
                              << " | LTP: " << std::fixed << std::setprecision(2) << (latest->last_price / 100.0)
                              << " | Bid: " << (latest->best_bid_price / 100.0)
                              << " | Ask: " << (latest->best_ask_price / 100.0) << std::flush;
                }
            }
        }
        std::cout << "\n";
    } else {
        auto config = hft::broker::AngelClient::load_config_from_env();
        if (token != 0) config.instrument_token = token;

        if (config.api_key.empty() || config.client_code.empty() || config.feed_token.empty()) {
            std::cerr << "\n[Error] Angel One credentials missing in environment!\n";
            std::cerr << "Required variables:\n";
            std::cerr << "  ANGEL_API_KEY       (e.g. your SmartAPI key)\n";
            std::cerr << "  ANGEL_CLIENT_CODE   (e.g. your client code)\n";
            std::cerr << "  ANGEL_FEED_TOKEN    (e.g. your active feed token)\n";
            std::cerr << "Optional variables:\n";
            std::cerr << "  ANGEL_JWT_TOKEN     (if session already logged in)\n\n";
            std::cerr << "Tip: You can test the full pipeline offline using: " << "live --mock\n";
            return 1;
        }

        hft::broker::AngelClient client(config);
        client.set_packet_callback([&pipeline](const uint8_t* data, size_t len) {
            hft::MarketEvent ev{};
            uint64_t recv_ts = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            if (hft::broker::AngelDecoder::decode(data, len, ev, recv_ts)) {
                pipeline.enqueue_event(ev);
            }
        });

        if (!client.connect()) {
            std::cerr << "Failed to connect to Angel One SmartStream.\n";
            return 1;
        }

        std::cout << "Connected to Angel One SmartStream. Subscribing to token " << config.instrument_token << "...\n";
        client.subscribe(config.instrument_token, hft::broker::AngelConstants::MODE_QUOTE);

        // Run network receive loop in background thread
        std::thread net_thread([&client]() {
            client.run_receive_loop();
        });

        auto last_report = std::chrono::steady_clock::now();
        uint64_t last_consumed = 0;

        while (!g_shutdown.load() && client.is_connected()) {
            if (duration_sec > 0) {
                auto curr = std::chrono::steady_clock::now();
                if (std::chrono::duration<double>(curr - stream_start).count() >= duration_sec) {
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            auto now = std::chrono::steady_clock::now();
            double elapsed = std::chrono::duration<double>(now - last_report).count();
            if (elapsed >= 1.0) {
                uint64_t current_consumed = pipeline.total_consumed();
                uint64_t rate = static_cast<uint64_t>((current_consumed - last_consumed) / elapsed);
                last_consumed = current_consumed;
                last_report = now;

                auto latest = pipeline.latest_event();
                if (latest) {
                    std::cout << "\r[ANGEL] Token: " << latest->instrument_token
                              << " | Events: " << current_consumed
                              << " (" << rate << " ev/s) | Drops: " << pipeline.total_dropped()
                              << " | LTP: " << std::fixed << std::setprecision(2) << (latest->last_price / 100.0)
                              << " | Bid: " << (latest->best_bid_price / 100.0)
                              << " | Ask: " << (latest->best_ask_price / 100.0) << std::flush;
                }
            }
        }

        client.stop();
        if (net_thread.joinable()) net_thread.join();
        client.disconnect();
        std::cout << "\nDisconnected.\n";
    }

    pipeline.stop_and_join();
    return 0;
}

int cmd_record(const std::string& path, bool mock_mode, size_t target_count) {
    std::cout << "Recording market data to " << path << " (" << (mock_mode ? "MOCK FEED" : "LIVE ANGEL ONE") << ")...\n";

    hft::MarketEventRecorder recorder;
    if (!recorder.open(path)) {
        std::cerr << "Failed to open file for recording: " << path << "\n";
        return 1;
    }

    hft::MarketDataPipeline pipeline;
    pipeline.set_event_listener([&recorder](const hft::MarketEvent& ev) {
        recorder.write(ev);
    });
    pipeline.start();

    std::signal(SIGINT, signal_handler);

    auto start_time = std::chrono::steady_clock::now();

    if (mock_mode) {
        auto packets = hft::broker::MockAngelFeed::generate_synthetic_stream(target_count);
        for (size_t i = 0; i < packets.size() && !g_shutdown.load(); ++i) {
            hft::MarketEvent ev{};
            uint64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            if (hft::broker::AngelDecoder::decode(packets[i].data(), packets[i].size(), ev, now_ns)) {
                pipeline.enqueue_event_wait(ev);
            }
        }
    } else {
        auto config = hft::broker::AngelClient::load_config_from_env();
        if (config.api_key.empty() || config.client_code.empty() || config.feed_token.empty()) {
            std::cerr << "Error: Angel One credentials missing in environment.\n";
            return 1;
        }

        hft::broker::AngelClient client(config);
        client.set_packet_callback([&pipeline](const uint8_t* data, size_t len) {
            hft::MarketEvent ev{};
            uint64_t recv_ts = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            if (hft::broker::AngelDecoder::decode(data, len, ev, recv_ts)) {
                pipeline.enqueue_event(ev);
            }
        });

        if (!client.connect()) {
            std::cerr << "Failed to connect to Angel One.\n";
            return 1;
        }

        client.subscribe(config.instrument_token, hft::broker::AngelConstants::MODE_QUOTE);
        std::thread net_thread([&client]() { client.run_receive_loop(); });

        while (!g_shutdown.load() && pipeline.total_consumed() < target_count) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        client.stop();
        if (net_thread.joinable()) net_thread.join();
        client.disconnect();
    }

    pipeline.stop_and_join();
    recorder.close();

    auto end_time = std::chrono::steady_clock::now();
    double sec = std::chrono::duration<double>(end_time - start_time).count();
    uintmax_t file_size = std::filesystem::file_size(path);

    std::cout << "\nRecording complete:\n";
    std::cout << "  File path:       " << path << "\n";
    std::cout << "  Events written:  " << recorder.events_written() << "\n";
    std::cout << "  File size:       " << file_size << " bytes (" << (file_size / (1024.0 * 1024.0)) << " MB)\n";
    std::cout << "  Duration:        " << (sec * 1000.0) << " ms\n";
    std::cout << "  Throughput:      " << static_cast<uint64_t>(recorder.events_written() / sec) << " events/sec\n";

    return 0;
}

int cmd_replay(const std::string& path) {
    std::cout << "Replaying recorded market data from " << path << " (OFFLINE)...\n";

    hft::MarketEventReplayer replayer;
    std::string err;
    if (!replayer.open(path, &err)) {
        std::cerr << "Failed to open market log: " << err << "\n";
        return 1;
    }

    if (!replayer.validate_full_checksum(&err)) {
        std::cerr << "CRC32 verification failed: " << err << "\n";
        return 1;
    }

    const auto& hdr = replayer.header();
    std::cout << "Header validated:\n";
    std::cout << "  Event count:     " << hdr.event_count << "\n";
    std::cout << "  Record size:     " << hdr.record_size << " bytes\n";
    std::cout << "  Data CRC32:      0x" << std::hex << hdr.data_crc32 << std::dec << "\n";

    uint64_t events_replayed = 0;
    int64_t min_price = 1000000000LL;
    int64_t max_price = 0;
    uint32_t token = 0;
    uint64_t first_ts = 0;
    uint64_t last_ts = 0;

    hft::MarketEvent ev{};
    auto start_time = std::chrono::high_resolution_clock::now();

    while (replayer.next(ev)) {
        if (events_replayed == 0) {
            token = ev.instrument_token;
            first_ts = ev.exchange_timestamp;
        }
        last_ts = ev.exchange_timestamp;
        if (ev.last_price < min_price && ev.last_price > 0) min_price = ev.last_price;
        if (ev.last_price > max_price) max_price = ev.last_price;
        ++events_replayed;
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    double duration_sec = std::chrono::duration<double>(end_time - start_time).count();

    replayer.close();

    std::cout << "Replay summary:\n";
    std::cout << "  Instrument:      " << token << "\n";
    std::cout << "  Events replayed: " << events_replayed << "\n";
    std::cout << "  Price range:     " << std::fixed << std::setprecision(2)
              << (min_price / 100.0) << " to " << (max_price / 100.0) << "\n";
    std::cout << "  Timespan:        " << ((last_ts - first_ts) / 1000000.0) << " ms\n";
    std::cout << "  Elapsed time:    " << (duration_sec * 1000.0) << " ms\n";
    std::cout << "  Throughput:      " << static_cast<uint64_t>(events_replayed / duration_sec) << " events/sec\n";

    return 0;
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    std::string mode = argv[1];

    if (mode == "live") {
        bool mock_mode = false;
        uint32_t token = 3045; // SBIN default
        uint32_t duration_sec = 0;
        for (int i = 2; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--mock") mock_mode = true;
            else if (arg == "--seconds" && i + 1 < argc) {
                duration_sec = static_cast<uint32_t>(std::stoul(argv[++i]));
            } else token = static_cast<uint32_t>(std::stoul(arg));
        }
        return cmd_live(mock_mode, token, duration_sec);
    } else if (mode == "record") {
        if (argc < 3) {
            print_usage(argv[0]);
            return 1;
        }
        std::string path = argv[2];
        bool mock_mode = false;
        size_t count = 50000;
        for (int i = 3; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--mock") mock_mode = true;
            else count = std::stoull(arg);
        }
        return cmd_record(path, mock_mode, count);
    } else if (mode == "replay") {
        if (argc < 3) {
            print_usage(argv[0]);
            return 1;
        }
        return cmd_replay(argv[2]);
    } else {
        std::cerr << "Unknown mode: " << mode << "\n\n";
        print_usage(argv[0]);
        return 1;
    }
}
