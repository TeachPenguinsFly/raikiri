#include <raikiri/introspect/queries.hpp>

#include <algorithm>
#include <utility>

namespace raikiri::introspect {

GetGatewayStats::GetGatewayStats(const GatewayStatsSource& source, WallClock now, std::uint64_t stale_after_ns)
    : source_{&source}, now_{std::move(now)}, stale_after_ns_{stale_after_ns} {}

GatewayView GetGatewayStats::view(SnapshotRecord record, std::uint64_t now_ns) const {
    const auto taken = record.stats.taken_ns;
    const auto state = classify(taken, record.process_alive, now_ns, stale_after_ns_);
    return GatewayView{
        .stats = std::move(record.stats), .state = state, .age_ms = now_ns > taken ? (now_ns - taken) / 1'000'000 : 0};
}

std::optional<GatewayView> GetGatewayStats::operator()(std::string_view account) const {
    auto record = source_->read(account);
    if (!record.has_value()) {
        return std::nullopt;
    }
    return view(std::move(*record), now_());
}

GetMachineStats::GetMachineStats(const GatewayStatsSource& source, WallClock now, std::uint64_t stale_after_ns)
    : one_{source, std::move(now), stale_after_ns} {}

MachineStats GetMachineStats::operator()() const {
    const auto now_ns = one_.now_();
    MachineStats out{.taken_ns = now_ns, .gateways = {}, .rollup = {}};
    for (auto& record : one_.source_->list()) {
        out.gateways.push_back(one_.view(std::move(record), now_ns));
    }
    std::ranges::sort(out.gateways, {}, [](const GatewayView& v) { return v.stats.account; });
    out.rollup = rollup(out.gateways);
    return out;
}

} // namespace raikiri::introspect
