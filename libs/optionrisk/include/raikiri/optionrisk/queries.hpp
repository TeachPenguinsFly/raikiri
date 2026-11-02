#pragma once

#include <raikiri/optiondata/queries.hpp>
#include <raikiri/optionrisk/risk.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace raikiri::optionrisk {

class HoldingSource {
public:
    HoldingSource() = default;
    HoldingSource(const HoldingSource&) = delete;
    HoldingSource& operator=(const HoldingSource&) = delete;
    virtual ~HoldingSource() = default;

    // replaces `out` with every option position the venues last reported as non-zero
    virtual void read(std::vector<Holding>& out) const = 0;

    // false when nothing can be said of the account, so a missing position is not a flat one
    [[nodiscard]] virtual bool attached(std::string_view account) const = 0;
};

struct BookedFill {
    InstrumentId instrument{};
    options::OptionKey key{};
    OptionUnit premium{OptionUnit::Unknown};
    FixedE9 realized{FixedE9::undef()};
};

class RealizedSource {
public:
    RealizedSource() = default;
    RealizedSource(const RealizedSource&) = delete;
    RealizedSource& operator=(const RealizedSource&) = delete;
    virtual ~RealizedSource() = default;

    // one row per contract, in its premium unit, flat ones included
    virtual void read(std::vector<BookedFill>& out) const = 0;
};

class SettlementSource {
public:
    SettlementSource() = default;
    SettlementSource(const SettlementSource&) = delete;
    SettlementSource& operator=(const SettlementSource&) = delete;
    virtual ~SettlementSource() = default;

    [[nodiscard]] virtual std::optional<SettlementPrice>
    price(VenueId venue, std::string_view underlying, UnixNanos expiry) const = 0;
};

// the venue's index read on the feed, the observation nearest after expiry; never one from before it
class IndexSettlements final : public SettlementSource {
public:
    explicit IndexSettlements(const optiondata::OptionMarket& market) : market_{&market} {}

    [[nodiscard]] std::optional<SettlementPrice>
    price(VenueId venue, std::string_view underlying, UnixNanos expiry) const override;

private:
    const optiondata::OptionMarket* market_;
    mutable std::vector<optiondata::OptionQuote> quotes_;
};

class SettlementLedger {
public:
    SettlementLedger() = default;
    SettlementLedger(const SettlementLedger&) = delete;
    SettlementLedger& operator=(const SettlementLedger&) = delete;
    virtual ~SettlementLedger() = default;

    [[nodiscard]] virtual std::vector<SettledPosition> load() = 0;
    virtual void append(const SettledPosition& settled) = 0;
};

struct RiskView {
    UnixNanos as_of{};
    std::vector<PositionRisk> positions{};
    // past expiry with no price to settle at: held out of the book and of every total, with no mark
    std::vector<Holding> awaiting_settlement{};
    std::vector<SettledPosition> settled{};
    Aggregates aggregates{};
    bool realized_known{false};
};

inline constexpr std::uint64_t k_realized_refresh_ns = 5'000'000'000ULL;

// remembers each open position across reads, because a venue clears an expired one on its next pull,
// usually before anything here has looked; a settled position is booked once, across restarts
class OptionRisk {
public:
    struct Sources {
        const HoldingSource* holdings{nullptr};
        const optiondata::OptionMarket* market{nullptr};
        const SettlementSource* settlements{nullptr};
        SettlementLedger* ledger{nullptr};
        const RealizedSource* realized{nullptr}; // may be null
    };

    OptionRisk(const Sources& sources, optiondata::WallClock now, Staleness staleness = {});

    // folds the venues' positions into the open set and settles what has expired
    void sweep();

    [[nodiscard]] RiskView view();

    [[nodiscard]] ScenarioMatrix
    scenarios(std::string_view underlying, std::span<const FixedE9> spot, std::span<const FixedE9> vol);

private:
    using Key = std::tuple<std::string, std::uint64_t>;

    void sweep_at(UnixNanos now);
    void refresh_realized(UnixNanos now);
    void assess_underlying(std::string_view underlying, UnixNanos now, std::vector<PositionRisk>& out);
    [[nodiscard]] bool booked(const Holding& holding) const;

    Sources sources_;
    optiondata::WallClock now_;
    Staleness staleness_;
    std::map<Key, Holding> open_{};
    std::vector<Holding> awaiting_{};
    std::vector<SettledPosition> settled_{};
    std::vector<BookedFill> realized_{};
    std::unordered_map<std::uint64_t, FixedE9> realized_by_id_{};
    std::uint64_t realized_due_{0};
    mutable std::vector<Holding> read_{};
    mutable std::vector<optiondata::OptionQuote> quotes_{};
};

} // namespace raikiri::optionrisk
