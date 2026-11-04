#include <raikiri/joe/parent_journal.hpp>
#include <raikiri/log/log.hpp>
#include <raikiri/oms/replay.hpp>
#include <raikiri/schema/codec.hpp>

#include <cstddef>
#include <glaze/glaze.hpp>
#include <span>

namespace raikiri::joe {

// named namespace: glaze reflection needs linkage
namespace parent_wire {

struct ChildRow {
    std::uint64_t id{0};
    std::uint64_t instrument{0};
    std::int64_t lot{0};
    std::int64_t lots{0};
    std::int64_t limit{0};
    std::uint64_t sent{0};
    std::uint8_t tif{0};
};

struct Row {
    std::uint8_t kind{0};
    std::uint64_t parent{0};
    std::uint64_t ts{0};
    std::string symbol{};
    std::uint8_t market{0};
    std::uint8_t side{0};
    std::int64_t spec_qty{0};
    std::int64_t notional{0};
    std::int64_t limit{0};
    std::uint8_t algo{0};
    std::uint64_t horizon_ns{0};
    std::uint32_t slices{0};
    std::uint32_t jitter_bp{0};
    std::uint32_t catchup_pct{0};
    std::uint8_t profile{0};
    std::uint32_t pov_bp{0};
    std::int64_t max_child_notional{0};
    std::uint32_t depth_share_bp{0};
    std::int64_t depth_band_bp{10 * k_e9};
    std::int64_t show{0};
    std::uint32_t variance_bp{0};
    std::uint8_t refill{0};
    std::uint8_t peg_to{0};
    std::int32_t offset_ticks{0};
    std::int32_t offset_bp{0};
    std::uint32_t threshold_ticks{0};
    std::uint64_t min_interval_ns{0};
    std::uint32_t venue{0};
    bool post_only{false};
    std::string client_tag{};
    std::int64_t qty{0};
    std::int64_t arrival{0};
    std::vector<std::uint64_t> times{};
    std::vector<std::uint64_t> weights{};
    std::uint64_t random{0};
    ChildRow child{};
    std::uint8_t status{0};
    std::string reason{};
};

} // namespace parent_wire

std::string encode_parent_record(const ParentRecord& record) {
    const auto& spec = record.spec;
    parent_wire::Row row{.kind = static_cast<std::uint8_t>(record.kind),
                         .parent = record.parent,
                         .ts = record.ts.ns,
                         .symbol = std::string{spec.listing.symbol.view()},
                         .market = static_cast<std::uint8_t>(spec.listing.market),
                         .side = static_cast<std::uint8_t>(spec.side),
                         .spec_qty = spec.qty.raw,
                         .notional = spec.notional.raw,
                         .limit = spec.limit.raw,
                         .algo = static_cast<std::uint8_t>(spec.algo),
                         .horizon_ns = spec.horizon_ns,
                         .slices = spec.slices,
                         .jitter_bp = spec.jitter_bp,
                         .catchup_pct = spec.catchup_pct,
                         .profile = static_cast<std::uint8_t>(spec.profile),
                         .pov_bp = spec.pov_bp,
                         .max_child_notional = spec.max_child_notional.raw,
                         .depth_share_bp = spec.depth_share_bp,
                         .depth_band_bp = spec.depth_band_bp.raw,
                         .show = spec.iceberg.show.raw,
                         .variance_bp = spec.iceberg.variance_bp,
                         .refill = static_cast<std::uint8_t>(spec.iceberg.refill),
                         .peg_to = static_cast<std::uint8_t>(spec.peg.to),
                         .offset_ticks = spec.peg.offset_ticks,
                         .offset_bp = spec.peg.offset_bp,
                         .threshold_ticks = spec.peg.threshold_ticks,
                         .min_interval_ns = spec.peg.min_interval_ns,
                         .venue = static_cast<std::uint32_t>(spec.venue),
                         .post_only = spec.post_only,
                         .client_tag = spec.client_tag,
                         .qty = record.qty.raw,
                         .arrival = record.arrival.raw,
                         .weights = record.weights,
                         .random = record.random,
                         .child = {.id = record.child.id,
                                   .instrument = record.child.instrument.packed,
                                   .lot = record.child.lot.raw,
                                   .lots = record.child.lots,
                                   .limit = record.child.limit.raw,
                                   .sent = record.child.sent.ns,
                                   .tif = static_cast<std::uint8_t>(record.child.tif)},
                         .status = static_cast<std::uint8_t>(record.status),
                         .reason = record.reason};
    row.times.reserve(record.times.size());
    for (const auto at : record.times) {
        row.times.push_back(at.ns);
    }
    std::string out;
    if (const auto ec = glz::write_json(row, out); ec) {
        return {};
    }
    return out;
}

std::optional<ParentRecord> decode_parent_record(std::string_view text) {
    parent_wire::Row row{};
    if (const auto ec = glz::read_json(row, std::string{text}); ec) {
        return std::nullopt;
    }
    ParentRecord out{.kind = static_cast<ParentRecord::Kind>(row.kind), .parent = row.parent, .ts = UnixNanos{row.ts}};
    auto& spec = out.spec;
    if (const auto symbol = SymbolText::from(row.symbol); symbol.has_value()) {
        spec.listing.symbol = *symbol;
    }
    spec.listing.market = static_cast<MarketType>(row.market);
    spec.side = static_cast<Side>(row.side);
    spec.qty = FixedE9::from_raw(row.spec_qty);
    spec.notional = FixedE9::from_raw(row.notional);
    spec.limit = FixedE9::from_raw(row.limit);
    spec.algo = static_cast<Algo>(row.algo);
    spec.horizon_ns = row.horizon_ns;
    spec.slices = row.slices;
    spec.jitter_bp = row.jitter_bp;
    spec.catchup_pct = row.catchup_pct;
    spec.profile = static_cast<Profile>(row.profile);
    spec.pov_bp = row.pov_bp;
    spec.max_child_notional = FixedE9::from_raw(row.max_child_notional);
    spec.depth_share_bp = row.depth_share_bp;
    spec.depth_band_bp = FixedE9::from_raw(row.depth_band_bp);
    spec.iceberg = {
        .show = FixedE9::from_raw(row.show), .variance_bp = row.variance_bp, .refill = static_cast<Refill>(row.refill)};
    spec.peg = {.to = static_cast<PegTo>(row.peg_to),
                .offset_ticks = row.offset_ticks,
                .offset_bp = row.offset_bp,
                .threshold_ticks = row.threshold_ticks,
                .min_interval_ns = row.min_interval_ns};
    spec.venue = static_cast<VenueId>(row.venue);
    spec.post_only = row.post_only;
    spec.client_tag = std::move(row.client_tag);
    out.qty = FixedE9::from_raw(row.qty);
    out.arrival = FixedE9::from_raw(row.arrival);
    out.times.reserve(row.times.size());
    for (const auto at : row.times) {
        out.times.push_back(UnixNanos{at});
    }
    out.weights = std::move(row.weights);
    out.random = row.random;
    out.child = {.id = row.child.id,
                 .instrument = InstrumentId{row.child.instrument},
                 .lot = FixedE9::from_raw(row.child.lot),
                 .lots = row.child.lots,
                 .limit = FixedE9::from_raw(row.child.limit),
                 .sent = UnixNanos{row.child.sent},
                 .tif = static_cast<ChildTif>(row.child.tif)};
    out.status = static_cast<Status>(row.status);
    out.reason = std::move(row.reason);
    return out;
}

void JournalParentLog::record(const ParentRecord& record) noexcept {
    try {
        const auto text = encode_parent_record(record);
        const std::span bytes{reinterpret_cast<const std::byte*>(text.data()), text.size()};
        if (const auto wrote = journal_->append(k_parent_record_schema, 1, bytes, record.ts.ns); !wrote.has_value()) {
            RLOG_ERROR("joe: parent {} did not reach the journal ({}, {} bytes); a restart will not know this change",
                       record.parent,
                       static_cast<int>(wrote.error()),
                       text.size());
        }
    } catch (...) {
        RLOG_ERROR("joe: parent {} did not reach the journal; a restart will not know this change", record.parent);
    }
}

std::size_t
restore_desk(Desk& desk, const std::string& journal_dir, const std::string& prefix, std::uint16_t strategy_id) {
    using msgs = raikiri_journal::schema::messages;
    std::size_t records = 0;
    oms::scan_journal(journal_dir, prefix, [&](const journal::FrameView& frame) {
        if (frame.schema_id == k_parent_record_schema) {
            const std::string_view text{reinterpret_cast<const char*>(frame.payload.data()), frame.payload.size()};
            if (const auto record = decode_parent_record(text); record.has_value()) {
                desk.restore(*record);
                ++records;
            }
        } else if (frame.schema_id == schema::message_id<msgs::OrderEventMsg>()) {
            const auto event = oms::decode_order_event(frame);
            if (event.client_order_id.strategy_id() == strategy_id) {
                desk.restore(event);
            }
        }
    });
    desk.finish_restore();
    return records;
}

} // namespace raikiri::joe
