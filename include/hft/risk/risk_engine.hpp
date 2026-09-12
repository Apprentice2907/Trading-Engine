#pragma once

#include "hft/order_command.hpp"
#include "hft/risk/risk_types.hpp"

namespace hft {

/**
 * @brief High-performance deterministic pre-trade risk engine.
 *
 * Enforces pre-trade risk checks (order sizing, price bands, notional value,
 * and aggregate exposure limits) on the hot path with 0 dynamic heap allocations.
 */
class PreTradeRiskEngine {
public:
    explicit PreTradeRiskEngine(RiskConfig config = RiskConfig{}) noexcept;
    ~PreTradeRiskEngine() = default;

    // Movable, non-copyable
    PreTradeRiskEngine(const PreTradeRiskEngine&) = delete;
    PreTradeRiskEngine& operator=(const PreTradeRiskEngine&) = delete;
    PreTradeRiskEngine(PreTradeRiskEngine&&) noexcept = default;
    PreTradeRiskEngine& operator=(PreTradeRiskEngine&&) noexcept = default;

    /**
     * @brief Performs pre-trade risk evaluation on an incoming order command.
     *
     * @param cmd Ingress order request
     * @return RiskResult indicating approval or specific rejection code
     */
    RiskResult check_order(const OrderCommand& cmd) noexcept;

    /**
     * @brief Updates state when an order is accepted and resting in the engine.
     */
    void on_order_approved(const OrderCommand& cmd) noexcept;

    /**
     * @brief Releases exposure when resting quantity is filled or partially filled.
     */
    void on_order_filled(Quantity fill_qty) noexcept;

    /**
     * @brief Releases exposure when a resting order is cancelled.
     */
    void on_order_cancelled(Quantity cancelled_qty) noexcept;

    /**
     * @brief Resets risk statistics and active exposure.
     */
    void reset() noexcept;

    // Accessors
    [[nodiscard]] const RiskConfig& config() const noexcept { return config_; }
    void set_config(const RiskConfig& cfg) noexcept { config_ = cfg; }
    [[nodiscard]] const RiskStats& stats() const noexcept { return stats_; }
    [[nodiscard]] Quantity current_exposure() const noexcept { return current_exposure_quantity_; }

private:
    RiskConfig config_;
    RiskStats  stats_{};
    Quantity   current_exposure_quantity_{0};
};

} // namespace hft
