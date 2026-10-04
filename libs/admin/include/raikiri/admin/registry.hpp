#pragma once

#include <raikiri/admin/types.hpp>
#include <raikiri/common/time.hpp>
#include <raikiri/domain/identifiers.hpp>
#include <raikiri/shm/records.hpp>
#include <raikiri/shm/segment.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace raikiri::admin {

// non-copyable because it owns a mapping
struct Gateway {
    AccountId account{};
    std::string name; // the segment name, which is also the account name
    shm::SegmentClient client;
    shm::GatewayChannels channels{};
};

class Registry {
public:
    Registry() = default;

    // attaches every gateway segment under `shm_dir`, skipping ones that refuse; also the rescan
    // re-attaches a gateway whose generation changed
    std::size_t discover(const std::string& shm_dir, std::int32_t reader_pid);

    // bumped on each re-attach; a holder of pointers into a segment must resolve again when it moves
    [[nodiscard]] std::uint64_t epoch() const noexcept { return epoch_; }

    // liveness uses the monotonic clock; the reported heartbeat is epoch nanos
    [[nodiscard]] std::vector<Account> accounts(MonoNanos now, UnixNanos wall) const;
    [[nodiscard]] Health health(MonoNanos now, UnixNanos wall) const;

    // clock and feed figures each gateway publishes in its segment
    [[nodiscard]] std::vector<VenueMetrics> venue_metrics(MonoNanos now, UnixNanos wall) const;

    // the catalogue of one gateway, or of all when `account` is empty
    [[nodiscard]] std::vector<Instrument> instruments(std::string_view account, std::optional<MarketType> market) const;

    // grouped by canonical symbol, filtered by case-insensitive substring, ordered by venue count then name
    [[nodiscard]] std::vector<Market> markets(std::string_view query, std::size_t limit) const;

    [[nodiscard]] std::optional<Instrument> instrument(InstrumentId id) const;

    // the listing gateway and catalogue row; pointers into the mapping, valid while this registry is
    struct CatalogueRow {
        const Gateway* gateway{nullptr};
        const raikiri::Instrument* instrument{nullptr};
    };

    [[nodiscard]] std::optional<CatalogueRow> catalogue_row(InstrumentId id) const noexcept;

    // a gateway that cannot report balances has empty assets and no `reported_at`
    [[nodiscard]] std::vector<AccountBalances> balances(std::string_view account, UnixNanos now) const;

    [[nodiscard]] const Gateway* find(std::string_view account) const noexcept;

    [[nodiscard]] std::size_t size() const noexcept { return gateways_.size(); }

    [[nodiscard]] const Gateway* at(std::size_t index) const noexcept {
        return index < gateways_.size() ? gateways_[index].get() : nullptr;
    }

private:
    [[nodiscard]] Gateway* find_mutable(std::string_view account) noexcept;

    // held by pointer so a gateway keeps its address across rescans and re-attaches
    std::vector<std::unique_ptr<Gateway>> gateways_;

    std::uint64_t epoch_{0};
};

// exposed for tests
[[nodiscard]] Instrument describe(const raikiri::Instrument& instrument, std::string_view account);

} // namespace raikiri::admin
