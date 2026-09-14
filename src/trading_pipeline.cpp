#include "hft/trading_pipeline.hpp"

namespace hft {

// ============================================================================
// PreTradeRiskEngine Implementation
// ============================================================================

PreTradeRiskEngine::PreTradeRiskEngine(RiskConfig config) noexcept
    : config_(config) {}

RiskResult PreTradeRiskEngine::check_order(const OrderCommand& cmd) noexcept {
    ++stats_.orders_checked;

    // Cancels strictly reduce or maintain risk
    if (cmd.type == EventType::Cancel) {
        if (cmd.order_id == 0) {
            ++stats_.orders_rejected;
            ++stats_.reject_invalid_quantity;
            return RiskResult{RiskCode::InvalidQuantity};
        }
        ++stats_.orders_approved;
        return RiskResult{RiskCode::Approved};
    }

    // 1. Order Validity
    if (cmd.order_id == 0) {
        ++stats_.orders_rejected;
        ++stats_.reject_invalid_quantity;
        return RiskResult{RiskCode::InvalidQuantity};
    }

    if (cmd.side != Side::Buy && cmd.side != Side::Sell) {
        ++stats_.orders_rejected;
        ++stats_.reject_invalid_side;
        return RiskResult{RiskCode::InvalidSide};
    }

    if (cmd.price <= 0) {
        ++stats_.orders_rejected;
        ++stats_.reject_invalid_price;
        return RiskResult{RiskCode::InvalidPrice};
    }

    if (cmd.qty == 0) {
        ++stats_.orders_rejected;
        ++stats_.reject_invalid_quantity;
        return RiskResult{RiskCode::InvalidQuantity};
    }

    // 2. Instrument Filter
    if (config_.allowed_instrument_id != 0 && cmd.instrument_id != config_.allowed_instrument_id) {
        ++stats_.orders_rejected;
        ++stats_.reject_invalid_instrument;
        return RiskResult{RiskCode::InvalidInstrument};
    }

    // 3. Price Protection (Bands)
    if (cmd.price < config_.min_price || cmd.price > config_.max_price) {
        ++stats_.orders_rejected;
        ++stats_.reject_price_band;
        return RiskResult{RiskCode::PriceBandViolation};
    }

    // 4. Maximum Order Quantity
    if (cmd.qty > config_.max_order_quantity) {
        ++stats_.orders_rejected;
        ++stats_.reject_max_quantity;
        return RiskResult{RiskCode::MaxQuantityExceeded};
    }

    // 5. Maximum Order Notional (price * quantity in fixed-point ticks)
    const uint64_t notional = static_cast<uint64_t>(cmd.price) * cmd.qty;
    if (notional > config_.max_order_notional) {
        ++stats_.orders_rejected;
        ++stats_.reject_max_notional;
        return RiskResult{RiskCode::MaxNotionalExceeded};
    }

    // 6. Aggregate Exposure Limit
    if (current_exposure_quantity_ + cmd.qty > config_.max_exposure_quantity) {
        ++stats_.orders_rejected;
        ++stats_.reject_exposure_limit;
        return RiskResult{RiskCode::ExposureLimitExceeded};
    }

    ++stats_.orders_approved;
    return RiskResult{RiskCode::Approved};
}

void PreTradeRiskEngine::on_order_approved(const OrderCommand& cmd) noexcept {
    if (cmd.type == EventType::Add) {
        current_exposure_quantity_ += cmd.qty;
    }
}

void PreTradeRiskEngine::on_order_filled(Quantity fill_qty) noexcept {
    if (current_exposure_quantity_ >= fill_qty) {
        current_exposure_quantity_ -= fill_qty;
    } else {
        current_exposure_quantity_ = 0;
    }
}

void PreTradeRiskEngine::on_order_cancelled(Quantity cancelled_qty) noexcept {
    if (current_exposure_quantity_ >= cancelled_qty) {
        current_exposure_quantity_ -= cancelled_qty;
    } else {
        current_exposure_quantity_ = 0;
    }
}

void PreTradeRiskEngine::reset() noexcept {
    stats_.reset();
    current_exposure_quantity_ = 0;
}

// ============================================================================
// OrderGateway Implementation
// ============================================================================

OrderGateway::OrderGateway(size_t trade_buffer_capacity) {
    trades_buf_.reserve(trade_buffer_capacity);
}

void OrderGateway::process_command(const OrderCommand& cmd, PreTradeRiskEngine& risk,
                                   MatchingEngine& engine, std::vector<ExecutionReport>& reports_out) {
    process_command(cmd, risk, engine, [&reports_out](const ExecutionReport& report) {
        reports_out.push_back(report);
    });
}

// ============================================================================
// OrderExecutionPipeline Implementation
// ============================================================================

OrderExecutionPipeline::OrderExecutionPipeline(size_t queue_capacity, RiskConfig risk_config)
    : ingress_queue_(std::make_unique<SpscQueue<OrderCommand, DEFAULT_QUEUE_CAPACITY, true>>()),
      egress_queue_(std::make_unique<SpscQueue<ExecutionReport, DEFAULT_QUEUE_CAPACITY, true>>()),
      risk_(risk_config),
      gateway_() {
    (void)queue_capacity;
}

OrderExecutionPipeline::~OrderExecutionPipeline() {
    if (running_.load(std::memory_order_relaxed)) {
        stop_and_join();
    }
}

void OrderExecutionPipeline::start() {
    if (running_.exchange(true, std::memory_order_acq_rel)) {
        return; // Already running
    }
    worker_thread_ = std::thread(&OrderExecutionPipeline::execution_loop, this);
}

void OrderExecutionPipeline::stop_and_join() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        return; // Already stopped
    }
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
}

bool OrderExecutionPipeline::submit_order(const OrderCommand& cmd) noexcept {
    if (ingress_queue_->try_push(cmd)) {
        orders_submitted_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    dropped_ingress_.fetch_add(1, std::memory_order_relaxed);
    return false;
}

void OrderExecutionPipeline::submit_order_wait(const OrderCommand& cmd) noexcept {
    while (!ingress_queue_->try_push(cmd)) {
        _mm_pause();
    }
    orders_submitted_.fetch_add(1, std::memory_order_relaxed);
}

bool OrderExecutionPipeline::poll_execution(ExecutionReport& rep) noexcept {
    return egress_queue_->try_pop(rep);
}

void OrderExecutionPipeline::execution_loop() {
    OrderCommand cmd{};
    while (running_.load(std::memory_order_relaxed)) {
        if (ingress_queue_->try_pop(cmd)) {
            process_one_command(cmd);
        } else {
            _mm_pause();
        }
    }

    // Drain all remaining orders before exiting
    while (ingress_queue_->try_pop(cmd)) {
        process_one_command(cmd);
    }
}

void OrderExecutionPipeline::process_one_command(const OrderCommand& cmd) {
    orders_processed_.fetch_add(1, std::memory_order_relaxed);
    gateway_.process_command(cmd, risk_, engine_, [this](const ExecutionReport& rep) {
        if (egress_queue_->try_push(rep)) {
            executions_emitted_.fetch_add(1, std::memory_order_relaxed);
        } else {
            dropped_egress_.fetch_add(1, std::memory_order_relaxed);
        }
    });
}

} // namespace hft
