#include <raikiri/oms/cross_record.hpp>

#include <array>
#include <cstddef>
#include <string_view>

namespace raikiri::oms {

ExecId cross_exec_id(ClientOrderId buy, ClientOrderId sell, Side side) noexcept {
    constexpr std::string_view digits = "0123456789abcdef";
    constexpr std::size_t k_hex = 16;
    std::array<char, 2 + k_hex + k_hex + 1> text{'i', 'x'};
    const auto write = [&](std::size_t at, std::uint64_t value) {
        for (std::size_t i = k_hex; i > 0; --i) {
            text[at + i - 1] = digits[value & 0xFU];
            value >>= 4U;
        }
    };
    write(2, buy.raw);
    write(2 + k_hex, sell.raw);
    text.back() = side == Side::Buy ? 'b' : 's';
    return ExecId::from(std::string_view{text.data(), text.size()}).value_or(ExecId{});
}

OrderEvent cross_leg_event(const CrossRecord& record, Side side) noexcept {
    const auto& leg = side == Side::Buy ? record.buy : record.sell;
    OrderEvent event{};
    event.type = OrderEventType::Filled;
    event.side = side;
    event.liquidity = Liquidity::Internal;
    event.instrument = record.instrument;
    event.client_order_id = leg.order;
    event.exec_id = cross_exec_id(record.buy.order, record.sell.order, side);
    event.price = record.price;
    event.qty = record.qty;
    event.leaves_qty = leg.leaves_after;
    event.fee = leg.fee;
    event.ts_event = record.ts;
    event.ts_init = record.ts;
    return event;
}

} // namespace raikiri::oms
