#include "hft/gateway/order_gateway.hpp"

namespace hft {

OrderGateway::OrderGateway(size_t trade_buffer_capacity) {
    trades_buf_.reserve(trade_buffer_capacity);
}

void OrderGateway::process_command(const OrderCommand& cmd, PreTradeRiskEngine& risk,
                                   MatchingEngine& engine, std::vector<ExecutionReport>& reports_out) {
    process_command(cmd, risk, engine, [&reports_out](const ExecutionReport& report) {
        reports_out.push_back(report);
    });
}

} // namespace hft
