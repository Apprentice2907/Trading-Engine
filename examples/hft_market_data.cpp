#define _CRT_SECURE_NO_WARNINGS

#include "hft/market_data.hpp"

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
    std::cout << "HFT Market Data Tool\n";
    std::cout << "Usage:\n";
    std::cout << "  " << prog << " live [--mock] [--yahoo SYMBOL] [--seconds N]     Stream live or mock market data\n";
    std::cout << "  " << prog << " record <output.mktlog> [--mock] [--yahoo SYMBOL] [count] Record market data to binary log\n";
    std::cout << "  " << prog << " replay <input.mktlog>                                  Replay recorded market data offline\n";
}

int cmd_live(bool mock_mode, const std::string& yahoo_symbol, uint32_t duration_sec = 0) {
    std::cout << "Starting market data stream (" << (mock_mode ? "MOCK FEED" : ("YAHOO FINANCE: " + yahoo_symbol)) << ")...\n";
    if (duration_sec > 0) {
        std::cout << "Running for controlled duration: " << duration_sec << " seconds...\n";
    }

    hft::MarketDataPipeline pipeline;
    pipeline.start();

    std::signal(SIGINT, signal_handler);
    auto stream_start = std::chrono::steady_clock::now();

    if (mock_mode) {
        std::cout << "Streaming deterministic mock ticks (Ctrl+C to stop)...\n";
        auto events = hft::MockMarketDataSource::generate_events(1000000, 3045);
        size_t idx = 0;

        auto last_report = std::chrono::steady_clock::now();
        uint64_t last_consumed = 0;

        while (!g_shutdown.load() && idx < events.size()) {
            if (duration_sec > 0) {
                auto curr = std::chrono::steady_clock::now();
                if (std::chrono::duration<double>(curr - stream_start).count() >= duration_sec) {
                    break;
                }
            }

            pipeline.enqueue_event(events[idx++]);
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
                              << " | Last: " << std::fixed << std::setprecision(2) << (latest->last_price / 100.0)
                              << " | Bid: " << (latest->best_bid_price / 100.0)
                              << " | Ask: " << (latest->best_ask_price / 100.0) << std::flush;
                }
            }
        }
        std::cout << "\n";
    } else {
        hft::YahooConfig config;
        config.symbols = {yahoo_symbol};
        config.poll_interval_ms = 1000;

        hft::YahooMarketDataSource source(config);
        source.attach_pipeline(&pipeline);

        std::cout << "Polling Yahoo Finance development feed for " << yahoo_symbol << " (best-effort, non-exchange-grade)...\n";
        if (!source.start()) {
            std::cerr << "Failed to start Yahoo market data source.\n";
            return 1;
        }

        auto last_report = std::chrono::steady_clock::now();
        uint64_t last_consumed = 0;

        while (!g_shutdown.load() && source.running()) {
            if (duration_sec > 0) {
                auto curr = std::chrono::steady_clock::now();
                if (std::chrono::duration<double>(curr - stream_start).count() >= duration_sec) {
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));

            auto now = std::chrono::steady_clock::now();
            double elapsed = std::chrono::duration<double>(now - last_report).count();
            if (elapsed >= 1.0) {
                uint64_t current_consumed = pipeline.total_consumed();
                last_consumed = current_consumed;
                last_report = now;

                auto latest = pipeline.latest_event();
                if (latest) {
                    const char* sym = reinterpret_cast<const char*>(latest->reserved);
                    std::cout << "\r[YAHOO] Symbol: " << (sym[0] ? sym : yahoo_symbol.c_str())
                              << " | Price: " << std::fixed << std::setprecision(2) << (latest->last_price / 100.0)
                              << " | Volume: " << latest->volume
                              << " | Events: " << current_consumed
                              << " | Drops: " << pipeline.total_dropped() << std::flush;
                }
            }
        }

        source.stop();
        std::cout << "\nDisconnected from Yahoo feed.\n";
    }

    pipeline.stop_and_join();
    return 0;
}

int cmd_record(const std::string& path, bool mock_mode, const std::string& yahoo_symbol, size_t target_count) {
    std::cout << "Recording market data to " << path << " (" << (mock_mode ? "MOCK FEED" : ("YAHOO FEED: " + yahoo_symbol)) << ")...\n";

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
        auto events = hft::MockMarketDataSource::generate_events(target_count, 3045);
        for (size_t i = 0; i < events.size() && !g_shutdown.load(); ++i) {
            pipeline.enqueue_event_wait(events[i]);
        }
    } else {
        hft::YahooConfig config;
        config.symbols = {yahoo_symbol};
        config.poll_interval_ms = 500;

        hft::YahooMarketDataSource source(config);
        source.attach_pipeline(&pipeline);
        source.start();

        while (!g_shutdown.load() && pipeline.total_consumed() < target_count) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        source.stop();
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
    if (sec > 0) {
        std::cout << "  Throughput:      " << static_cast<uint64_t>(recorder.events_written() / sec) << " events/sec\n";
    }

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
    if (duration_sec > 0) {
        std::cout << "  Throughput:      " << static_cast<uint64_t>(events_replayed / duration_sec) << " events/sec\n";
    }

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
        std::string yahoo_symbol = "AAPL";
        uint32_t duration_sec = 0;

        for (int i = 2; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--mock") {
                mock_mode = true;
            } else if (arg == "--yahoo") {
                mock_mode = false;
                if (i + 1 < argc && argv[i + 1][0] != '-') {
                    yahoo_symbol = argv[++i];
                }
            } else if (arg == "--seconds" && i + 1 < argc) {
                duration_sec = static_cast<uint32_t>(std::stoul(argv[++i]));
            } else if (!arg.empty() && arg[0] != '-') {
                // Positional argument: if numeric and mock_mode was set, it's token; else symbol
                if (!mock_mode) {
                    yahoo_symbol = arg;
                }
            }
        }
        return cmd_live(mock_mode, yahoo_symbol, duration_sec);
    } else if (mode == "record") {
        if (argc < 3) {
            print_usage(argv[0]);
            return 1;
        }
        std::string path = argv[2];
        bool mock_mode = false;
        std::string yahoo_symbol = "AAPL";
        size_t count = 50;

        for (int i = 3; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--mock") {
                mock_mode = true;
            } else if (arg == "--yahoo") {
                mock_mode = false;
                if (i + 1 < argc && argv[i + 1][0] != '-') {
                    yahoo_symbol = argv[++i];
                }
            } else {
                try {
                    count = std::stoull(arg);
                } catch (...) {
                    if (!mock_mode) {
                        yahoo_symbol = arg;
                    }
                }
            }
        }
        return cmd_record(path, mock_mode, yahoo_symbol, count);
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
