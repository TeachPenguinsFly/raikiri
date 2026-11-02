#include <raikiri/journal/reader.hpp>
#include <raikiri/oms/replay.hpp>
#include <raikiri/schema/codec.hpp>

#include <algorithm>

namespace raikiri::oms {

namespace internal {

using msgs = raikiri_journal::schema::messages;

template <typename Fixed, typename Field> [[nodiscard]] Fixed fixed_text(Field field) {
    const std::string_view raw{reinterpret_cast<const char*>(field.data()), field.size()};
    const auto stop = raw.find('\0');
    const auto text = raw.substr(0, stop);
    return Fixed::from(text).value_or(Fixed{});
}

} // namespace internal

void OrderReplay::on_cmd(const OrderCmd& cmd) {
    // a long journal closes far more orders than anyone can still owe a fill on
    if ((++commands_ & 0xFFFU) == 0) {
        path_->closed().prune(cmd.ts_created, k_closed_order_window_ns);
    }
    const auto owner = cmd.client_order_id.strategy_id();
    last_seq_[owner] = std::max(last_seq_[owner], cmd.client_order_id.seq());
    OrderEvent local{};
    local.client_order_id = cmd.client_order_id;
    local.instrument = cmd.instrument;
    local.side = cmd.side;
    local.price = cmd.price;
    local.qty = cmd.qty;
    local.ts_init = cmd.ts_created;
    switch (cmd.kind) {
    case CmdKind::Create:
        if (const auto created = store_->create(cmd, cmd.exchange_client_order_id); !created.has_value()) {
            dropped_ += created.error() == StoreError::Full ? 1U : 0U;
            return;
        }
        local.type = OrderEventType::Submitted;
        break;
    case CmdKind::Cancel:
        local.type = OrderEventType::PendingCancel;
        break;
    case CmdKind::Amend:
        local.type = OrderEventType::PendingUpdate;
        break;
    }
    (void)store_->apply_event(local);
}

void OrderReplay::on_event(OrderEvent event) {
    ++events_;
    const bool pulled = event.type == OrderEventType::Filled && event.leaves_qty.is_undef();
    if (pulled && !path_->seen().admit(event.exec_id.view(), event.ts_init)) {
        return;
    }
    (void)path_->apply(event, pulled, event.ts_init);
}

std::uint64_t OrderReplay::last_seq(std::uint16_t strategy_id) const noexcept {
    const auto it = last_seq_.find(strategy_id);
    return it == last_seq_.end() ? 0 : it->second;
}

void append_order_cmd(journal::JournalWriter& writer,
                      const OrderCmd& cmd,
                      const OrderOrigin& origin,
                      BookKind kind) noexcept {
    (void)schema::append_msg<internal::msgs::OrderCmdMsg>(writer, cmd.ts_created.ns, [&](auto m) {
        namespace jt = raikiri_journal::types;
        m.kind(static_cast<jt::cmdKindEnum>(cmd.kind));
        m.side(static_cast<jt::sideEnum>(cmd.side));
        m.orderType(static_cast<jt::orderTypeEnum>(cmd.order_type));
        m.tif(static_cast<jt::tifEnum>(cmd.tif));
        m.flags(static_cast<std::uint8_t>(cmd.flags));
        m.instrument(jt::instrumentId{cmd.instrument.packed});
        m.clientOrderIdField(jt::clientOrderId{cmd.client_order_id.raw});
        m.exchangeClientOrderIdField().assign_range(cmd.exchange_client_order_id.view());
        m.exchangeOrderIdField().assign_range(cmd.target_exchange_order_id.view());
        m.price(jt::priceTicks{cmd.price.raw});
        m.qty(jt::qtyLots{cmd.qty.raw});
        m.tsCreated(jt::timestampNs{cmd.ts_created.ns});
        m.bookKind(static_cast<std::uint8_t>(kind));
        m.parentId(jt::clientOrderId{origin.parent});
        m.externalRef().assign_range(origin.external_ref.view());
    });
}

void append_order_event(journal::JournalWriter& writer, const OrderEvent& event) noexcept {
    (void)schema::append_msg<internal::msgs::OrderEventMsg>(writer, event.ts_init.ns, [&](auto m) {
        namespace jt = raikiri_journal::types;
        m.eventType(static_cast<jt::orderEventTypeEnum>(event.type));
        m.side(static_cast<jt::sideEnum>(event.side));
        m.orderType(static_cast<jt::orderTypeEnum>(event.order_type));
        m.tif(static_cast<jt::tifEnum>(event.tif));
        m.flags(static_cast<std::uint8_t>(event.flags));
        m.liquidity(static_cast<jt::liquidityEnum>(event.liquidity));
        m.instrument(jt::instrumentId{event.instrument.packed});
        m.clientOrderIdField(jt::clientOrderId{event.client_order_id.raw});
        m.exchangeClientOrderIdField().assign_range(event.exchange_client_order_id.view());
        m.exchangeOrderIdField().assign_range(event.exchange_order_id.view());
        m.execIdField().assign_range(event.exec_id.view());
        m.price(jt::priceTicks{event.price.raw});
        m.qty(jt::qtyLots{event.qty.raw});
        m.leavesQty(jt::qtyLots{event.leaves_qty.raw});
        m.fee(jt::fixedE9{event.fee.raw});
        m.tsEvent(jt::timestampNs{event.ts_event.ns});
        m.tsInit(jt::timestampNs{event.ts_init.ns});
        m.rejectClass(static_cast<jt::errorClassEnum>(event.reject_class));
        m.rejectCode(jt::venueCode{event.reject_code});
        m.feeAsset().assign_range(event.fee_asset.view());
    });
}

OrderEvent decode_order_event(const journal::FrameView& frame) {
    const auto m = schema::view_msg<internal::msgs::OrderEventMsg>(frame.payload);
    OrderEvent out{};
    out.type = static_cast<OrderEventType>(sbepp::to_underlying(m.eventType()));
    out.side = static_cast<Side>(sbepp::to_underlying(m.side()));
    out.order_type = static_cast<OrderType>(sbepp::to_underlying(m.orderType()));
    out.tif = static_cast<TimeInForce>(sbepp::to_underlying(m.tif()));
    out.flags = static_cast<OrderFlags>(*m.flags());
    out.liquidity = static_cast<Liquidity>(sbepp::to_underlying(m.liquidity()));
    out.instrument = InstrumentId{m.instrument().value()};
    out.client_order_id = ClientOrderId{m.clientOrderIdField().value()};
    out.exchange_client_order_id = internal::fixed_text<ExchangeClientOrderId>(m.exchangeClientOrderIdField());
    out.exchange_order_id = internal::fixed_text<ExchangeOrderId>(m.exchangeOrderIdField());
    out.exec_id = internal::fixed_text<ExecId>(m.execIdField());
    out.price = PriceTicks::from_raw(m.price().value());
    out.qty = QtyLots::from_raw(m.qty().value());
    out.leaves_qty = QtyLots::from_raw(m.leavesQty().value());
    out.fee = FixedE9::from_raw(m.fee().value());
    out.ts_event = UnixNanos{m.tsEvent().value()};
    out.ts_init = UnixNanos{m.tsInit().value()};
    // records before version 6 are shorter
    if (frame.schema_version >= 6) {
        out.fee_asset = internal::fixed_text<FeeAsset>(m.feeAsset());
    }
    return out;
}

OrderCmd decode_order_cmd(const journal::FrameView& frame) {
    const auto m = schema::view_msg<internal::msgs::OrderCmdMsg>(frame.payload);
    OrderCmd out{};
    out.kind = static_cast<CmdKind>(sbepp::to_underlying(m.kind()));
    out.side = static_cast<Side>(sbepp::to_underlying(m.side()));
    out.order_type = static_cast<OrderType>(sbepp::to_underlying(m.orderType()));
    out.tif = static_cast<TimeInForce>(sbepp::to_underlying(m.tif()));
    out.flags = static_cast<OrderFlags>(*m.flags());
    out.instrument = InstrumentId{m.instrument().value()};
    out.client_order_id = ClientOrderId{m.clientOrderIdField().value()};
    out.exchange_client_order_id = internal::fixed_text<ExchangeClientOrderId>(m.exchangeClientOrderIdField());
    out.target_exchange_order_id = internal::fixed_text<ExchangeOrderId>(m.exchangeOrderIdField());
    out.price = PriceTicks::from_raw(m.price().value());
    out.qty = QtyLots::from_raw(m.qty().value());
    out.ts_created = UnixNanos{m.tsCreated().value()};
    return out;
}

void scan_journal(const std::string& dir,
                  const std::string& prefix,
                  const std::function<void(const journal::FrameView&)>& visit) {
    auto reader = journal::JournalReader::open(dir, prefix);
    if (!reader.has_value()) {
        return;
    }
    journal::FrameView frame{};
    while (reader->next(frame) == journal::ReadResult::Item) {
        visit(frame);
    }
}

void replay_orders(const std::string& dir, const std::string& prefix, OrderReplay& replay) {
    scan_journal(dir, prefix, [&replay](const journal::FrameView& frame) {
        if (frame.schema_id == schema::message_id<internal::msgs::OrderCmdMsg>()) {
            replay.on_cmd(decode_order_cmd(frame));
        } else if (frame.schema_id == schema::message_id<internal::msgs::OrderEventMsg>()) {
            replay.on_event(decode_order_event(frame));
        }
    });
}

} // namespace raikiri::oms
