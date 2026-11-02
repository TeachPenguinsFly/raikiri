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
    if (cmd.client_order_id.is_internal_leg()) {
        last_internal_leg_ = std::max(last_internal_leg_,
                                      cmd.client_order_id.seq() & ~ClientOrderId::k_internal_leg_bit);
    } else {
        last_seq_[owner] = std::max(last_seq_[owner], cmd.client_order_id.seq());
    }
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
    if (event.liquidity == Liquidity::Internal && event.type == OrderEventType::Filled) {
        echoed_.push_back(event.exec_id);
        return;
    }
    const bool pulled = event.type == OrderEventType::Filled && event.leaves_qty.is_undef();
    if (pulled && !path_->seen().admit(event.exec_id.view(), event.ts_init)) {
        return;
    }
    (void)path_->apply(event, pulled, event.ts_init);
}

void OrderReplay::on_cross(const CrossRecord& record) {
    ++events_;
    for (const auto side : {Side::Buy, Side::Sell}) {
        auto event = cross_leg_event(record, side);
        (void)path_->apply(event, false, event.ts_init);
    }
    crosses_.push_back(record);
}

std::vector<OrderEvent> OrderReplay::unechoed_legs() const {
    std::vector<OrderEvent> out;
    const auto echoed = [this](const ExecId& id) { return std::ranges::find(echoed_, id) != echoed_.end(); };
    // a crash can only part the newest cross from its frames
    if (crosses_.empty()) {
        return out;
    }
    for (const auto side : {Side::Buy, Side::Sell}) {
        auto event = cross_leg_event(crosses_.back(), side);
        if (!echoed(event.exec_id)) {
            out.push_back(event);
        }
    }
    return out;
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

void append_cross(journal::JournalWriter& writer, const CrossRecord& record) noexcept {
    (void)schema::append_msg<internal::msgs::InternalCrossMsg>(writer, record.ts.ns, [&](auto m) {
        namespace jt = raikiri_journal::types;
        m.instrument(jt::instrumentId{record.instrument.packed});
        m.symbol().assign_range(record.symbol.view());
        m.price(jt::priceTicks{record.price.raw});
        m.qty(jt::qtyLots{record.qty.raw});
        m.refBid(jt::fixedE9{record.ref_bid.raw});
        m.refAsk(jt::fixedE9{record.ref_ask.raw});
        m.notional(jt::fixedE9{record.notional.raw});
        m.saved(jt::fixedE9{record.saved.raw});
        m.tsEvent(jt::timestampNs{record.ts.ns});
        m.buyOrder(jt::clientOrderId{record.buy.order.raw});
        m.buyLeaves(jt::qtyLots{record.buy.leaves_after.raw});
        m.buyFee(jt::fixedE9{record.buy.fee.raw});
        m.buyNetQty(jt::qtyLots{record.buy.net_qty.raw});
        m.buyCost(jt::fixedE9{record.buy.cost.raw});
        m.buyRealized(jt::fixedE9{record.buy.realized.raw});
        m.buyFees(jt::fixedE9{record.buy.fees.raw});
        m.buyFillCount(record.buy.fill_count);
        m.buyTsOpened(jt::timestampNs{record.buy.ts_opened.ns});
        m.sellOrder(jt::clientOrderId{record.sell.order.raw});
        m.sellLeaves(jt::qtyLots{record.sell.leaves_after.raw});
        m.sellFee(jt::fixedE9{record.sell.fee.raw});
        m.sellNetQty(jt::qtyLots{record.sell.net_qty.raw});
        m.sellCost(jt::fixedE9{record.sell.cost.raw});
        m.sellRealized(jt::fixedE9{record.sell.realized.raw});
        m.sellFees(jt::fixedE9{record.sell.fees.raw});
        m.sellFillCount(record.sell.fill_count);
        m.sellTsOpened(jt::timestampNs{record.sell.ts_opened.ns});
        m.sellIsDesk(static_cast<std::uint8_t>(record.sell.desk ? 1 : 0));
        m.buyIsDesk(static_cast<std::uint8_t>(record.buy.desk ? 1 : 0));
    });
}

bool is_cross_frame(const journal::FrameView& frame) noexcept {
    return frame.schema_id == schema::message_id<internal::msgs::InternalCrossMsg>();
}

CrossRecord decode_cross(const journal::FrameView& frame) {
    const auto m = schema::view_msg<internal::msgs::InternalCrossMsg>(frame.payload);
    CrossRecord out{};
    out.instrument = InstrumentId{m.instrument().value()};
    out.symbol = internal::fixed_text<SymbolText>(m.symbol());
    out.price = PriceTicks::from_raw(m.price().value());
    out.qty = QtyLots::from_raw(m.qty().value());
    out.ref_bid = FixedE9::from_raw(m.refBid().value());
    out.ref_ask = FixedE9::from_raw(m.refAsk().value());
    out.notional = FixedE9::from_raw(m.notional().value());
    out.saved = FixedE9::from_raw(m.saved().value());
    out.ts = UnixNanos{m.tsEvent().value()};
    out.buy = {.order = ClientOrderId{m.buyOrder().value()},
               .leaves_after = QtyLots::from_raw(m.buyLeaves().value()),
               .fee = FixedE9::from_raw(m.buyFee().value()),
               .net_qty = QtyLots::from_raw(m.buyNetQty().value()),
               .cost = FixedE9::from_raw(m.buyCost().value()),
               .realized = FixedE9::from_raw(m.buyRealized().value()),
               .fees = FixedE9::from_raw(m.buyFees().value()),
               .fill_count = *m.buyFillCount(),
               .ts_opened = UnixNanos{m.buyTsOpened().value()},
               .desk = *m.buyIsDesk() != 0};
    out.sell = {.order = ClientOrderId{m.sellOrder().value()},
                .leaves_after = QtyLots::from_raw(m.sellLeaves().value()),
                .fee = FixedE9::from_raw(m.sellFee().value()),
                .net_qty = QtyLots::from_raw(m.sellNetQty().value()),
                .cost = FixedE9::from_raw(m.sellCost().value()),
                .realized = FixedE9::from_raw(m.sellRealized().value()),
                .fees = FixedE9::from_raw(m.sellFees().value()),
                .fill_count = *m.sellFillCount(),
                .ts_opened = UnixNanos{m.sellTsOpened().value()},
                .desk = *m.sellIsDesk() != 0};
    return out;
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
        } else if (is_cross_frame(frame)) {
            replay.on_cross(decode_cross(frame));
        }
    });
}

} // namespace raikiri::oms
