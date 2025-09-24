#pragma once

// Every gateway this machine is running, and what each of them can answer.
//
// Discovery is a directory listing: gatewayd names its segment after the
// account it serves — `gw.bybit`, `gw.bybit-spot`, `gw.deribit-opt` — so the
// set of running gateways is the set of files, and no registration protocol,
// service discovery or config file has to agree with reality.
//
// A REGISTRY IS NOT A CACHE. It holds attachments, not answers: every read
// goes to the segment, because the whole point of the shared-memory design is
// that the current value is one acquire-load away. Caching here would put a
// second, staler answer beside the true one.

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

// One attached gateway. Non-copyable because it owns a mapping.
struct Gateway {
    AccountId account{};
    std::string name; // the segment name, which is also the account name
    shm::SegmentClient client;
    shm::GatewayChannels channels{};
};

class Registry {
public:
    Registry() = default;

    // Attaches to every gateway segment under `shm_dir`. Returns how many are
    // attached in total; a segment that refuses — wrong ABI, wrong layout, a
    // dead owner — is skipped rather than fatal, because one bad gateway must
    // not stop the reader from seeing the rest.
    //
    // SAFE TO CALL REPEATEDLY, and meant to be: this is the rescan as well as
    // the first look — and it has to be, because the segment file exists from
    // the instant a gateway creates it but its channels appear only once it has
    // published, so a reader that looked exactly once would miss every gateway
    // that was still starting, and every gateway started afterwards for the
    // rest of its life.
    //
    // A GATEWAY THAT RESTARTED IS RE-ATTACHED, and the segment's generation is
    // what says so: it is the owner's start time, so one generation is one run
    // of one gateway. An attachment whose generation still matches the file on
    // disk is left exactly as it is; one whose generation moved belongs to a
    // process that is gone. Measured before this existed — a gatewayd was
    // killed and started again, loaded its instruments and subscribed, and
    // /v1/health went on reporting that account alive=false with the dead run's
    // generation until admind itself was restarted.
    std::size_t discover(const std::string& shm_dir, std::int32_t reader_pid);

    // How many attachments have been REPLACED, which is the signal a holder of
    // any pointer into a segment needs. A catalogue row, a channel, the tables
    // behind them: all of them belong to the mapping they were resolved
    // against, and a re-attach unmaps it. Anything that keeps one across a
    // discover() must compare this against what it captured and resolve again
    // — Stream's live subscriptions do, and the alternative was refusing to
    // re-attach while a client was connected, which would leave the venue dead
    // for exactly as long as one terminal stays open.
    [[nodiscard]] std::uint64_t epoch() const noexcept { return epoch_; }

    [[nodiscard]] std::vector<Account> accounts(MonoNanos now) const;
    [[nodiscard]] Health health(MonoNanos now) const;

    // The catalogue of one gateway, or of all of them when `account` is empty.
    [[nodiscard]] std::vector<Instrument> instruments(std::string_view account,
                                                      std::optional<MarketType> market) const;

    // Every market the attached gateways list, grouped by canonical symbol and
    // narrowed by `query` — a case-insensitive substring of that symbol.
    //
    // Ordered by HOW MANY VENUES list it, then by name. A search for "BTC"
    // should answer with the market eight venues quote before the one that a
    // single venue lists, because the first is what a cross-venue view is for.
    [[nodiscard]] std::vector<Market> markets(std::string_view query, std::size_t limit) const;

    [[nodiscard]] std::optional<Instrument> instrument(InstrumentId id) const;

    // The gateway that lists an instrument, and the catalogue row itself.
    //
    // A writer needs both and the DTO above gives neither: the row carries the
    // tick and lot sizes a decimal has to be converted through, and the
    // gateway carries the ring the command goes into. Pointers into the
    // mapping, valid while this Registry is.
    struct CatalogueRow {
        const Gateway* gateway{nullptr};
        const raikiri::Instrument* instrument{nullptr};
    };

    [[nodiscard]] std::optional<CatalogueRow> catalogue_row(InstrumentId id) const noexcept;

    // What each attached gateway's venue last said the account holds, or just
    // one of them when `account` names it. A gateway whose venue cannot report
    // balances is present with an empty asset list and `reported_at` absent —
    // the two are different facts and the caller must be able to tell them
    // apart.
    [[nodiscard]] std::vector<AccountBalances> balances(std::string_view account,
                                                        UnixNanos now) const;

    [[nodiscard]] const Gateway* find(std::string_view account) const noexcept;

    [[nodiscard]] std::size_t size() const noexcept { return gateways_.size(); }

    // Positional access, for a caller that has to reach every attached
    // gateway rather than one it can name.
    [[nodiscard]] const Gateway* at(std::size_t index) const noexcept {
        return index < gateways_.size() ? gateways_[index].get() : nullptr;
    }

private:
    [[nodiscard]] Gateway* find_mutable(std::string_view account) noexcept;

    // HELD BY POINTER SO THE GATEWAYS DO NOT MOVE. A live stream subscription
    // keeps a `const Gateway*` per instrument it is watching, and this used to
    // be a vector of values: every rescan push_back'd — reallocating — and
    // then SORTED, which moves elements even when it does not reallocate. Any
    // client subscribed while a gateway appeared was left reading a gateway
    // that had been moved out from under it, or freed memory. Sorting a vector
    // of pointers rearranges only the pointers.
    //
    // A re-attach keeps that promise as well: it assigns the new mapping over
    // the Gateway in place, so a `const Gateway*` stays a pointer to a live
    // object and only the segment behind it changes. What it cannot keep alive
    // is a pointer INTO the old mapping, which is what `epoch()` is for.
    std::vector<std::unique_ptr<Gateway>> gateways_;

    std::uint64_t epoch_{0};
};

// Exposed for tests and for anything that needs one row without a segment.
[[nodiscard]] Instrument describe(const raikiri::Instrument& instrument, std::string_view account);

} // namespace raikiri::admin
