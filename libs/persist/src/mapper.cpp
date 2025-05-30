#include <raikiri/domain/identifiers.hpp>
#include <raikiri/domain/order_events.hpp>
#include <raikiri/persist/mapper.hpp>
#include <raikiri/schema/codec.hpp>

#include <string>

namespace raikiri::persist {

namespace {

using msgs = raikiri_journal::schema::messages;

std::string venue_of(std::uint64_t packed_instrument) {
    return std::string{to_string(InstrumentId{packed_instrument}.venue())};
}

// Fixed-length SBE char fields are space/NUL padded; trim to the real value.
std::string trim_fixed(const auto& array_ref) {
    const std::string_view raw{array_ref.data(), array_ref.size()};
    const auto end = raw.find_last_not_of(std::string_view{"\0 ", 2});
    return end == std::string_view::npos ? std::string{} : std::string{raw.substr(0, end + 1)};
}

template <typename Enum> std::string enum_name(Enum e, std::string_view (*to_str)(std::uint8_t)) {
    return std::string{to_str(static_cast<std::uint8_t>(sbepp::to_underlying(e)))};
}

std::string_view event_name(std::uint8_t v) {
    return to_string(static_cast<OrderEventType>(v));
}

std::string_view side_name(std::uint8_t v) {
    return to_string(static_cast<Side>(v));
}

std::string_view liquidity_name(std::uint8_t v) {
    return to_string(static_cast<Liquidity>(v));
}

std::string_view pnl_kind_name(raikiri_journal::types::pnlKindEnum kind) {
    using K = raikiri_journal::types::pnlKindEnum;
    switch (kind) {
    case K::Realized:
        return "realized";
    case K::Fee:
        return "fee";
    case K::Funding:
        return "funding";
    }
    return "realized";
}

// Event → projection status. Deliberately partial: events that need FSM
// context (Updated, ModifyRejected, CancelRejected, ReconGap) leave the
// projection status untouched; the event log carries the full story.
std::string status_for(raikiri_journal::types::orderEventTypeEnum type, std::int64_t leaves) {
    using E = raikiri_journal::types::orderEventTypeEnum;
    switch (type) {
    case E::Initialized:
        return "initialized";
    case E::Denied:
        return "denied";
    case E::Submitted:
        return "submitted";
    case E::Accepted:
        return "accepted";
    case E::Rejected:
        return "rejected";
    case E::Triggered:
        return "triggered";
    case E::PendingUpdate:
        return "pending_update";
    case E::PendingCancel:
        return "pending_cancel";
    case E::Canceled:
    case E::ReconClosed:
        return "canceled";
    case E::Expired:
        return "expired";
    case E::Filled:
        return leaves == 0 ? "filled" : "partially_filled";
    case E::Updated:
    case E::ModifyRejected:
    case E::CancelRejected:
    case E::ReconGap:
        return {};
    }
    return {};
}

void map_order_event(const journal::FrameView& frame, RowBatch& batch) {
    const auto m = schema::view_msg<msgs::OrderEventMsg>(frame.payload);
    const auto type_name = enum_name(m.eventType(), +[](std::uint8_t v) { return event_name(v); });
    const auto exec_id = trim_fixed(m.execIdField());

    batch.order_events.push_back({
        .journal_seq = frame.seq,
        .client_order_id = m.clientOrderIdField().value(),
        .event_type = type_name,
        .instrument_id = m.instrument().value(),
        .price_ticks = m.price().value(),
        .qty_lots = m.qty().value(),
        .leaves_lots = m.leavesQty().value(),
        .fee_e9 = m.fee().value(),
        .exec_id = exec_id,
        .ts_event = m.tsEvent().value(),
        .ts_recorded = m.tsInit().value(),
    });

    batch.order_upserts.push_back({
        .client_order_id = m.clientOrderIdField().value(),
        .exchange_client_order_id = trim_fixed(m.exchangeClientOrderIdField()),
        .exchange_order_id = trim_fixed(m.exchangeOrderIdField()),
        .instrument_id = m.instrument().value(),
        .side = enum_name(
            m.side(), +[](std::uint8_t v) { return side_name(v); }),
        .order_type =
            m.orderType() == raikiri_journal::types::orderTypeEnum::Market ? "market" : "limit",
        .tif = m.tif() == raikiri_journal::types::tifEnum::Ioc   ? "ioc"
               : m.tif() == raikiri_journal::types::tifEnum::Fok ? "fok"
                                                                 : "gtc",
        .flags = static_cast<std::int16_t>(m.flags().value()),
        .price_ticks = m.price().value(),
        .qty_lots = m.qty().value(),
        .leaves_lots = m.leavesQty().value(),
        .status = status_for(m.eventType(), m.leavesQty().value()),
        .ts_event = m.tsEvent().value(),
    });

    if (m.eventType() == raikiri_journal::types::orderEventTypeEnum::Filled && !exec_id.empty()) {
        batch.executions.push_back({
            .venue = venue_of(m.instrument().value()),
            .exec_id = exec_id,
            .exchange_order_id = trim_fixed(m.exchangeOrderIdField()),
            .client_order_id = m.clientOrderIdField().value(),
            .instrument_id = m.instrument().value(),
            .side = enum_name(
                m.side(), +[](std::uint8_t v) { return side_name(v); }),
            .price_ticks = m.price().value(),
            .qty_lots = m.qty().value(),
            .fee_e9 = m.fee().value(),
            .liquidity = enum_name(
                m.liquidity(), +[](std::uint8_t v) { return liquidity_name(v); }),
            .ts_event = m.tsEvent().value(),
        });
    }
}

} // namespace

void map_frame(const journal::FrameView& frame, RowBatch& batch) {
    if (frame.schema_id == schema::message_id<msgs::OrderEventMsg>()) {
        map_order_event(frame, batch);
    } else if (frame.schema_id == schema::message_id<msgs::PnlIncrementMsg>()) {
        const auto m = schema::view_msg<msgs::PnlIncrementMsg>(frame.payload);
        const auto venue = venue_of(m.instrument().value());
        const auto kind = pnl_kind_name(m.kind());
        const auto exec_id = trim_fixed(m.execIdField());
        batch.pnl_increments.push_back({
            .source_id = std::string{kind} + ":" + venue + ":" + exec_id,
            .kind = std::string{kind},
            .settle_side = m.settleSide() == raikiri_journal::types::settleSideEnum::Base ? "base"
                                                                                         : "quote",
            .strategy_id = m.strategyId().value(),
            .venue = venue,
            .exchange_symbol = trim_fixed(m.symbol()),
            .instrument_id = m.instrument().value(),
            .client_order_id = m.clientOrderIdField().value(),
            .amount_e9 = m.amount().value(),
            .ts_event = m.tsEvent().value(),
        });
    } else if (frame.schema_id == schema::message_id<msgs::ReconBreakMsg>()) {
        const auto m = schema::view_msg<msgs::ReconBreakMsg>(frame.payload);
        using V = raikiri_journal::types::reconVerdictEnum;
        batch.recon_breaks.push_back({
            .venue = std::string{to_string(static_cast<VenueId>(sbepp::to_underlying(m.venue())))},
            .exchange_symbol = trim_fixed(m.symbol()),
            .ts_seen = m.tsSeen().value(),
            .local_qty_lots = m.localQty().value(),
            .venue_qty_lots = m.venueQty().value(),
            .detail = m.verdict() == V::Unknown ? "venue could not report" : "quantities differ",
        });
    } else if (frame.schema_id == schema::message_id<msgs::FundingMsg>()) {
        const auto m = schema::view_msg<msgs::FundingMsg>(frame.payload);
        batch.funding.push_back({
            .instrument_id = m.instrument().value(),
            .ts_event = m.tsEvent().value(),
            .rate_e9 = m.rate().value(),
            .predicted_e9 = m.predictedRate().value(),
            .mark_ticks = m.markPrice().value(),
            .next_funding_ts = m.nextFundingTime().value(),
            .interval_s = m.intervalS().value(),
        });
    } else if (frame.schema_id == schema::message_id<msgs::ConnectivityMsg>()) {
        const auto m = schema::view_msg<msgs::ConnectivityMsg>(frame.payload);
        using CE = raikiri_journal::types::connEventEnum;
        batch.connectivity.push_back({
            .journal_seq = frame.seq,
            .venue = std::string{to_string(static_cast<VenueId>(sbepp::to_underlying(m.venue())))},
            .event = m.event() == CE::Connected      ? "connected"
                     : m.event() == CE::Disconnected ? "disconnected"
                     : m.event() == CE::AuthFailed   ? "auth_failed"
                                                     : "resubscribed",
            .stream_id = m.streamId().value(),
            .ts_recorded = m.tsInit().value(),
        });
    } else if (frame.schema_id == schema::message_id<msgs::LatencySampleMsg>()) {
        const auto m = schema::view_msg<msgs::LatencySampleMsg>(frame.payload);
        batch.latency.push_back({
            .journal_seq = frame.seq,
            .venue = std::string{to_string(static_cast<VenueId>(sbepp::to_underlying(m.venue())))},
            .metric = m.metric().value(),
            .p50_ns = m.p50Ns().value(),
            .p99_ns = m.p99Ns().value(),
            .p999_ns = m.p999Ns().value(),
            .max_ns = m.maxNs().value(),
            .count = m.count().value(),
            .ts_recorded = m.tsInit().value(),
        });
    }
    batch.last_seq = frame.seq;
}

} // namespace raikiri::persist
