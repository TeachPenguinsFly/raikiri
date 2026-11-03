#include <raikiri/portfolio/journal.hpp>
#include <raikiri/portfolio/pnl.hpp>

#include <algorithm>
#include <array>
#include <string_view>

namespace raikiri::portfolio {

namespace internal {
using msgs = raikiri_journal::schema::messages;
} // namespace internal

void append_pnl(journal::JournalWriter& writer,
                const OrderEvent& event,
                const Instrument& listed,
                raikiri_journal::types::pnlKindEnum kind,
                FixedE9 amount) noexcept {
    if (amount.raw == 0 || amount.is_undef()) {
        return; // an increment of nothing is not a fact worth keeping
    }
    const auto& base = base_of(listed);
    (void)schema::append_msg<internal::msgs::PnlIncrementMsg>(writer, event.ts_event.ns, [&](auto m) {
        namespace jt = raikiri_journal::types;
        m.kind(kind);
        m.settleSide(settle_side(listed) == SettleSide::Base ? jt::settleSideEnum::Base : jt::settleSideEnum::Quote);
        m.strategyId(event.client_order_id.strategy_id());
        m.instrument(jt::instrumentId{event.instrument.packed});
        m.symbol().assign_range(base.exchange_symbol.view());
        m.clientOrderIdField(jt::clientOrderId{event.client_order_id.raw});
        m.execIdField().assign_range(
            fill_identity(event.client_order_id, event.leaves_qty, event.exec_id.view()).view());
        m.amount(jt::fixedE9{amount.raw});
        m.tsEvent(jt::timestampNs{event.ts_event.ns});
    });
}

void append_desk_fee(journal::JournalWriter& writer,
                     std::uint16_t desk_book,
                     const OrderEvent& event,
                     const Instrument& listed,
                     FixedE9 amount) noexcept {
    if (amount.raw == 0 || amount.is_undef()) {
        return;
    }
    const auto& base = base_of(listed);
    const auto fill = fill_identity(event.client_order_id, event.leaves_qty, event.exec_id.view());
    // a cross's id leaves room for the prefix, and only a cross has a desk fee
    std::array<char, ExecId{}.data.size()> identity{'d', '.'};
    const auto length = std::min(fill.view().size(), identity.size() - 2);
    std::copy_n(fill.view().data(), length, identity.data() + 2);
    (void)schema::append_msg<internal::msgs::PnlIncrementMsg>(writer, event.ts_event.ns, [&](auto m) {
        namespace jt = raikiri_journal::types;
        m.kind(jt::pnlKindEnum::Fee);
        m.settleSide(settle_side(listed) == SettleSide::Base ? jt::settleSideEnum::Base : jt::settleSideEnum::Quote);
        m.strategyId(desk_book);
        m.instrument(jt::instrumentId{event.instrument.packed});
        m.symbol().assign_range(base.exchange_symbol.view());
        m.clientOrderIdField(jt::clientOrderId{event.client_order_id.raw});
        m.execIdField().assign_range(std::string_view{identity.data(), 2 + length});
        m.amount(jt::fixedE9{amount.raw});
        m.tsEvent(jt::timestampNs{event.ts_event.ns});
    });
}

void append_position(journal::JournalWriter& writer, const Position& position, const Instrument& listed) noexcept {
    const auto& base = base_of(listed);
    (void)schema::append_msg<internal::msgs::PositionMsg>(writer, position.ts_updated().ns, [&](auto m) {
        namespace jt = raikiri_journal::types;
        m.strategyId(position.key().strategy_id);
        m.settleSide(settle_side(listed) == SettleSide::Base ? jt::settleSideEnum::Base : jt::settleSideEnum::Quote);
        m.venue(static_cast<jt::venueIdEnum>(position.key().instrument.venue()));
        m.instrument(jt::instrumentId{position.key().instrument.packed});
        m.symbol().assign_range(base.exchange_symbol.view());
        m.netQty(jt::qtyLots{position.qty().raw});
        // store cost; the average is derived
        m.cost(jt::fixedE9{position.cost().raw});
        m.realized(jt::fixedE9{position.realized().raw});
        m.fees(jt::fixedE9{position.fees().raw});
        m.fillCount(position.fill_count());
        m.tsOpened(jt::timestampNs{position.ts_opened().ns});
        m.tsUpdated(jt::timestampNs{position.ts_updated().ns});
    });
}

} // namespace raikiri::portfolio
