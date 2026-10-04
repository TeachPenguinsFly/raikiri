#pragma once

#include <raikiri/domain/asset_class.hpp>
#include <raikiri/domain/instrument.hpp>
#include <raikiri/exec/data_client.hpp>
#include <raikiri/exec/rate_budget.hpp>
#include <raikiri/introspect/queries.hpp>
#include <raikiri/net/ws_limits.hpp>
#include <raikiri/net/ws_pool.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace raikiri::gatewayd {

enum class Channel : std::uint8_t { Bbo = 0, Trades, Depth, Funding, Ticker, Greeks, Count };

inline constexpr std::array<std::string_view, static_cast<std::size_t>(Channel::Count)> k_channel_names{
    "bbo", "trades", "depth", "funding", "ticker", "greeks"};

struct SubTally {
    std::uint64_t requested{0};
    std::uint64_t accepted{0};
    std::uint64_t refused{0};
};

using SubTallies = std::array<SubTally, static_cast<std::size_t>(Channel::Count)>;
using MessageCounts = introspect::RateWindow::Counts;

struct Coverage {
    std::uint64_t priced_60s{0};
    std::uint64_t priced_ever{0};
    std::uint64_t books_seeded{0};
    std::uint64_t funding_received{0};
};

struct Snapshot {
    std::uint64_t wall_ns{0};
    std::uint64_t mono_ns{0};
    MessageCounts messages{};
    Coverage coverage;
    std::uint64_t venue_refusals{0};
    std::uint64_t broken_frames{0};
    std::vector<introspect::RestEndpointStats> rest_endpoints;
    introspect::ClockStats clock;
    std::vector<introspect::LatencyStats> latency;
};

[[nodiscard]] inline std::string_view kind_name(raikiri::ContractKind kind) noexcept {
    switch (kind) {
    case raikiri::ContractKind::Linear:
        return "linear";
    case raikiri::ContractKind::Inverse:
        return "inverse";
    case raikiri::ContractKind::Unknown:
        break;
    }
    return "unknown";
}

inline void tally(std::vector<introspect::Tally>& into, std::string_view name) {
    auto it = std::ranges::find(into, name, &introspect::Tally::name);
    if (it == into.end()) {
        it = into.insert(into.end(), introspect::Tally{.name = std::string{name}, .count = 0});
    }
    ++it->count;
}

class StatsCollector {
public:
    static constexpr std::uint64_t k_window_ns = 60ULL * 1'000'000'000ULL;

    StatsCollector(introspect::GatewayStats identity, introspect::StatsPublisher& publisher)
        : base_{std::move(identity)}, publisher_{&publisher} {}

    void catalogue(std::span<const raikiri::Instrument> instruments) {
        auto& out = base_.catalogue;
        out = {};
        out.instruments = instruments.size();
        for (const auto& instrument : instruments) {
            const auto& base = raikiri::base_of(instrument);
            tally(out.by_market, raikiri::to_string(base.id.market()));
            tally(out.by_asset_class, raikiri::to_string(base.asset_class));
            tally(out.by_kind, kind_name(base.kind));
        }
    }

    void subscriptions(const SubTallies& tallies) { subs_ = tallies; }

    void limits(const raikiri::net::WsLimits& limits) {
        base_.limits = introspect::SocketLimits{.configured = limits.configured,
                                                .ws_streams = limits.streams_per_socket,
                                                .ws_sockets = limits.sockets,
                                                .ws_channels = limits.channels,
                                                .ws_subscriptions_per_frame = limits.subscriptions_per_frame,
                                                .ws_socket_messages = limits.socket_messages.max,
                                                .ws_socket_window_ms = limits.socket_messages.window_ns / 1'000'000,
                                                .ws_connects = limits.connects.max,
                                                .ws_connects_window_ms = limits.connects.window_ns / 1'000'000};
    }

    bool publish(Snapshot now, raikiri::exec::RateBudget* budget) {
        window_.push(now.mono_ns, now.messages);
        auto stats = base_;
        stats.taken_ns = now.wall_ns;
        stats.process.uptime_s = (now.wall_ns - stats.process.started_ns) / 1'000'000'000ULL;

        for (std::size_t i = 0; i < k_channel_names.size(); ++i) {
            const auto& sub = subs_[i];
            if (sub.requested == 0 && now.messages[i] == 0) {
                continue;
            }
            stats.subscriptions.push_back(introspect::SubscriptionStats{.channel = std::string{k_channel_names[i]},
                                                                        .requested = sub.requested,
                                                                        .accepted = sub.accepted,
                                                                        .refused = sub.refused,
                                                                        .messages = now.messages[i],
                                                                        .per_second = window_.per_second(i)});
        }

        for (auto& pool : raikiri::net::process_ws_pool_stats()) {
            stats.sockets.push_back(introspect::SocketStats{
                .host = std::move(pool.host),
                .sockets = pool.sockets,
                .open = pool.open,
                .keys = pool.keys,
                .min_keys = pool.min_keys,
                .avg_keys = pool.sockets == 0 ? 0.0
                                              : static_cast<double>(pool.keys) / static_cast<double>(pool.sockets),
                .max_keys = pool.max_keys,
                .pending_frames = pool.pending_frames,
                .reopens = pool.reopens,
                .last_close_reason = std::move(pool.last_close_reason),
                .last_close_ns = pool.last_close_ns,
                .backoffs = pool.backoffs,
                .refusals = pool.refusals,
                .backoff_ms = pool.backoff_ms});
        }

        const auto bbo = subs_[static_cast<std::size_t>(Channel::Bbo)].accepted;
        const auto depth = subs_[static_cast<std::size_t>(Channel::Depth)].accepted;
        stats.coverage = introspect::CoverageStats{
            .subscribed = bbo,
            .priced_60s = now.coverage.priced_60s,
            .priced_ever = now.coverage.priced_ever,
            .books_seeded = now.coverage.books_seeded,
            .books_unseeded = depth > now.coverage.books_seeded ? depth - now.coverage.books_seeded : 0,
            .funding_received = now.coverage.funding_received};

        stats.rates.messages_per_second = window_.total_per_second();
        stats.rates.window_s = k_window_ns / 1'000'000'000ULL;
        stats.rates.venue_refusals = now.venue_refusals;
        stats.rates.broken_frames = now.broken_frames;
        stats.rates.rest_endpoints = std::move(now.rest_endpoints);
        stats.clock = std::move(now.clock);
        stats.latency = std::move(now.latency);
        if (budget != nullptr) {
            stats.rates.budget_denials = budget->denials();
            for (auto& limit : budget->usage()) {
                stats.rates.rest_limits.push_back(introspect::RestLimitStats{.name = std::move(limit.name),
                                                                             .max = limit.max,
                                                                             .window_ms = limit.window_ns / 1'000'000,
                                                                             .calls = limit.calls,
                                                                             .denied = limit.denied});
            }
        }
        return publisher_->publish(stats);
    }

private:
    introspect::GatewayStats base_;
    introspect::StatsPublisher* publisher_;
    SubTallies subs_{};
    introspect::RateWindow window_{k_window_ns};
};

} // namespace raikiri::gatewayd
