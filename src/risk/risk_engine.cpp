#include "hft/risk/risk_engine.hpp"

namespace hft {

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

} // namespace hft
