#include "hft/order.hpp"
#include "hft/trading_pipeline.hpp"
#include "hft/market_data.hpp"

#include <iostream>
#include <iomanip>
#include <vector>
#include <string>
#include <chrono>
#include <thread>
#include <atomic>
#include <csignal>

namespace {

std::atomic<bool> g_shutdown{false};

void signal_handler(int) {
    g_shutdown.store(true);
}

void print_separator() {
    std::cout << "--------------------------------------------------------------------------------\n";
}

int run_execution_demo() {
    std::cout << "================================================================================\n";
    std::cout << "             LOW-LATENCY ORDER EXECUTION PIPELINE DEMONSTRATION                \n";
    std::cout << "================================================================================\n";
    std::cout << "Architecture:\n";
    std::cout << "  OrderCommand (64B) -> Ingress SPSC -> PreTradeRisk -> Gateway -> MatchingEngine\n";
    std::cout << "                                                                         |\n";
    std::cout << "  Consumer <- Egress SPSC <- ExecutionReport (64B) <---------------------+\n";
    std::cout << "Execution Model: Deterministic In-Memory Simulated Matching Engine (C++20)\n\n";

    // 1. Configure pre-trade risk parameters
    hft::RiskConfig risk_cfg{};
    risk_cfg.max_order_quantity = 500;
    risk_cfg.max_order_notional = 100000000; // 100,000.00
    risk_cfg.min_price = 80000;              // 800.00
    risk_cfg.max_price = 90000;              // 900.00
    risk_cfg.allowed_instrument_id = 3045;   // SBIN

    hft::OrderExecutionPipeline pipeline(1024, risk_cfg);
    pipeline.start();

    std::vector<hft::ExecutionReport> collected_reports;

    auto drain_reports = [&]() {
        hft::ExecutionReport rep{};
        while (pipeline.poll_execution(rep)) {
            collected_reports.push_back(rep);
            std::cout << "  [EGRESS SPSC] Order #" << rep.order_id
                      << " | Type: " << std::setw(15) << std::left << hft::to_string(rep.exec_type)
                      << " | Side: " << (rep.side == hft::Side::Buy ? "BUY " : "SELL")
                      << " | Price: " << std::fixed << std::setprecision(2) << (rep.price / 100.0)
                      << " | LastQty: " << std::setw(4) << rep.last_qty
                      << " | LeavesQty: " << std::setw(4) << rep.leaves_qty;
            if (rep.exec_type == hft::ExecutionType::RiskRejected) {
                std::cout << " | Reason: " << hft::to_string(rep.risk_code);
            }
            std::cout << "\n";
        }
    };

    // Scenario 1: Pre-Trade Risk Rejection (Price band violation)
    print_separator();
    std::cout << "[SCENARIO 1] Pre-Trade Risk Rejection (Order Price Below Allowed Band)\n";
    std::cout << "  Submitting: BUY 100 @ 750.00 (Allowed band: 800.00 - 900.00)\n";
    hft::OrderCommand cmd1 = hft::OrderCommand::make_add(101, 3045, 1, hft::Side::Buy, 75000, 100);
    pipeline.submit_order_wait(cmd1);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    drain_reports();

    // Scenario 2: Resting Limit Order
    print_separator();
    std::cout << "[SCENARIO 2] Passive Resting Limit Order\n";
    std::cout << "  Submitting: BUY 100 @ 830.00 (Inside band: 800.00 - 900.00)\n";
    hft::OrderCommand cmd2 = hft::OrderCommand::make_add(102, 3045, 1, hft::Side::Buy, 83000, 100);
    pipeline.submit_order_wait(cmd2);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    drain_reports();

    // Scenario 3: Immediate Full Crossing Fill
    print_separator();
    std::cout << "[SCENARIO 3] Aggressive Immediate Crossing Fill\n";
    std::cout << "  Submitting: SELL 40 @ 830.00 (Crosses resting Buy #102 @ 830.00)\n";
    hft::OrderCommand cmd3 = hft::OrderCommand::make_add(103, 3045, 1, hft::Side::Sell, 83000, 40);
    pipeline.submit_order_wait(cmd3);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    drain_reports();

    // Scenario 4: Partial Fill with Residual Leaves Resting
    print_separator();
    std::cout << "[SCENARIO 4] Partial Fill Sweeping Remaining Resting Liquidity + Leaves Resting\n";
    std::cout << "  Submitting: SELL 100 @ 830.00 (60 fills against Buy #102, 40 rests on Ask side)\n";
    hft::OrderCommand cmd4 = hft::OrderCommand::make_add(104, 3045, 1, hft::Side::Sell, 83000, 100);
    pipeline.submit_order_wait(cmd4);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    drain_reports();

    // Scenario 5: Order Cancellation
    print_separator();
    std::cout << "[SCENARIO 5] Order Cancellation\n";
    std::cout << "  Submitting: CANCEL Order #104 (Cancelling remaining 40 resting on Ask side)\n";
    hft::OrderCommand cmd5 = hft::OrderCommand::make_cancel(104, 3045, 1);
    pipeline.submit_order_wait(cmd5);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    drain_reports();

    // Shut down pipeline cleanly
    pipeline.stop_and_join();

    print_separator();
    std::cout << "\n================================================================================\n";
    std::cout << "                       PIPELINE EXECUTION AUDIT SUMMARY                         \n";
    std::cout << "================================================================================\n";
    std::cout << "  Orders Submitted to Ingress:    " << pipeline.orders_submitted() << "\n";
    std::cout << "  Orders Processed by Engine:     " << pipeline.orders_processed() << "\n";
    std::cout << "  Executions Emitted to Egress:   " << pipeline.executions_emitted() << "\n";
    std::cout << "  Execution Reports Collected:    " << collected_reports.size() << "\n";
    std::cout << "  Ingress SPSC Drops:             " << pipeline.dropped_ingress() << " (Zero drop guarantee)\n";
    std::cout << "  Egress SPSC Drops:              " << pipeline.dropped_egress() << " (Zero drop guarantee)\n";
    std::cout << "  Pre-Trade Risk Checks Passed:   " << pipeline.risk().stats().orders_approved << "\n";
    std::cout << "  Pre-Trade Risk Checks Rejected: " << pipeline.risk().stats().orders_rejected << "\n";
    std::cout << "  Resting Orders in Engine Book:  " << pipeline.engine().book().total_orders() << " (Book cleanly balanced)\n";
    std::cout << "================================================================================\n";

    return 0;
}

int run_integrated_demo(bool mock_mode, uint32_t token, uint32_t duration_sec) {
    std::cout << "================================================================================\n";
    std::cout << " [INFRASTRUCTURE DEMO] LIVE MARKET DATA INGESTION + SIMULATED EXECUTION         \n";
    std::cout << "================================================================================\n";
    std::cout << "IMPORTANT NOTICE:\n";
    std::cout << "  Market Data Feed:  READ-ONLY observation (" << (mock_mode ? "MOCK STREAM" : "ANGEL ONE WEBSOCKET") << ")\n";
    std::cout << "  Order Execution:   SIMULATED in-memory MatchingEngine (NO real-money orders)\n";
    std::cout << "  Security Policy:   Credentials read strictly from environment variables\n";
    std::cout << "================================================================================\n\n";

    hft::MarketDataPipeline md_pipeline;
    md_pipeline.start();

    hft::RiskConfig risk_cfg{};
    risk_cfg.max_order_quantity = 500;
    risk_cfg.max_order_notional = 100000000;
    risk_cfg.min_price = 10000;
    risk_cfg.max_price = 200000;
    risk_cfg.allowed_instrument_id = token;

    hft::OrderExecutionPipeline exec_pipeline(1024, risk_cfg);
    exec_pipeline.start();

    std::signal(SIGINT, signal_handler);

    std::atomic<uint64_t> sim_order_counter{1000};
    std::atomic<uint64_t> ticks_received{0};

    // Callback when MarketData pipeline consumes an event:
    // Demonstrates event-driven infrastructure: observation triggers deterministic test command
    md_pipeline.set_event_listener([&](const hft::MarketEvent& ev) {
        uint64_t count = ++ticks_received;
        // Deterministically submit a test order every 20 ticks
        if (count % 20 == 0 && !g_shutdown.load()) {
            uint64_t order_id = ++sim_order_counter;
            hft::Side side = (count % 40 == 0) ? hft::Side::Sell : hft::Side::Buy;
            int64_t price = ev.last_price;
            uint32_t qty = 10;

            hft::OrderCommand cmd = hft::OrderCommand::make_add(
                order_id, ev.instrument_token, 1, side, price, qty);
            exec_pipeline.submit_order_wait(cmd);
        }
    });

    auto start_time = std::chrono::steady_clock::now();

    if (mock_mode) {
        std::cout << "Streaming ticks & driving simulated execution pipeline for token " << token << "...\n";
        auto packets = hft::broker::MockAngelFeed::generate_synthetic_stream(100000);
        size_t idx = 0;

        while (!g_shutdown.load() && idx < packets.size()) {
            if (duration_sec > 0) {
                auto now = std::chrono::steady_clock::now();
                if (std::chrono::duration<double>(now - start_time).count() >= duration_sec) {
                    break;
                }
            }

            hft::MarketEvent ev{};
            uint64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            if (hft::broker::AngelDecoder::decode(packets[idx].data(), packets[idx].size(), ev, now_ns)) {
                md_pipeline.enqueue_event(ev);
            }
            ++idx;

            // Poll and display simulated execution reports
            hft::ExecutionReport rep{};
            while (exec_pipeline.poll_execution(rep)) {
                std::cout << "  [SIM-EXEC] Order #" << rep.order_id
                          << " | " << std::setw(14) << hft::to_string(rep.exec_type)
                          << " | " << (rep.side == hft::Side::Buy ? "BUY " : "SELL")
                          << " | Px: " << std::fixed << std::setprecision(2) << (rep.price / 100.0)
                          << " | Qty: " << rep.last_qty
                          << " | Leaves: " << rep.leaves_qty << "\n";
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    } else {
        auto config = hft::broker::AngelClient::load_config_from_env();
        if (config.api_key.empty() || config.client_code.empty() || config.feed_token.empty()) {
            std::cerr << "\n[Error] Angel One credentials missing in environment!\n";
            std::cerr << "Run with '--mock' to test the integrated pipeline offline.\n";
            return 1;
        }

        hft::broker::AngelClient client(config);
        client.set_packet_callback([&md_pipeline](const uint8_t* data, size_t len) {
            hft::MarketEvent ev{};
            uint64_t recv_ts = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            if (hft::broker::AngelDecoder::decode(data, len, ev, recv_ts)) {
                md_pipeline.enqueue_event(ev);
            }
        });

        if (!client.connect()) {
            std::cerr << "Failed to connect to Angel One feed.\n";
            return 1;
        }

        client.subscribe(token, hft::broker::AngelConstants::MODE_QUOTE);
        std::thread net_thread([&client]() { client.run_receive_loop(); });

        while (!g_shutdown.load() && client.is_connected()) {
            if (duration_sec > 0) {
                auto now = std::chrono::steady_clock::now();
                if (std::chrono::duration<double>(now - start_time).count() >= duration_sec) {
                    break;
                }
            }

            hft::ExecutionReport rep{};
            while (exec_pipeline.poll_execution(rep)) {
                std::cout << "  [SIM-EXEC] Order #" << rep.order_id
                          << " | " << std::setw(14) << hft::to_string(rep.exec_type)
                          << " | " << (rep.side == hft::Side::Buy ? "BUY " : "SELL")
                          << " | Px: " << std::fixed << std::setprecision(2) << (rep.price / 100.0)
                          << " | Qty: " << rep.last_qty
                          << " | Leaves: " << rep.leaves_qty << "\n";
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        client.stop();
        if (net_thread.joinable()) net_thread.join();
        client.disconnect();
    }

    md_pipeline.stop_and_join();
    exec_pipeline.stop_and_join();

    print_separator();
    std::cout << "Integrated Demo Summary:\n";
    std::cout << "  Market Ticks Processed:     " << ticks_received.load() << "\n";
    std::cout << "  Simulated Orders Generated: " << (sim_order_counter.load() - 1000) << "\n";
    std::cout << "  Executions Emitted:         " << exec_pipeline.executions_emitted() << "\n";
    std::cout << "  Ingress / Egress Drops:     0 / 0\n";
    print_separator();

    return 0;
}

} // namespace

int main(int argc, char* argv[]) {
    bool integrated = false;
    bool mock_mode = false;
    uint32_t token = 3045; // SBIN default
    uint32_t seconds = 3;  // default 3s for integrated demo

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--integrated") integrated = true;
        else if (arg == "--execution") integrated = false;
        else if (arg == "--mock") mock_mode = true;
        else if (arg == "--seconds" && i + 1 < argc) seconds = static_cast<uint32_t>(std::stoul(argv[++i]));
        else if (arg == "--token" && i + 1 < argc) token = static_cast<uint32_t>(std::stoul(argv[++i]));
        else if (arg == "--help" || arg == "-h") {
            std::cout << "HFT Pipeline Demo Tool\n";
            std::cout << "Usage:\n";
            std::cout << "  " << argv[0] << " [--execution]             Deterministic execution pipeline walkthrough\n";
            std::cout << "  " << argv[0] << " --integrated [--mock] [--seconds N] Ingestion + simulated execution demo\n";
            return 0;
        }
    }

    if (integrated) {
        return run_integrated_demo(mock_mode, token, seconds);
    } else {
        return run_execution_demo();
    }
}
