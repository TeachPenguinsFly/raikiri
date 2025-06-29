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

    // Attaches to every gateway segment under `shm_dir`. Returns how many
    // attached; a segment that refuses — wrong ABI, wrong layout, a dead
    // owner — is skipped rather than fatal, because one bad gateway must not
    // stop the reader from seeing the rest.
    std::size_t discover(const std::string& shm_dir, std::int32_t reader_pid);

    [[nodiscard]] std::vector<Account> accounts(MonoNanos now) const;
    [[nodiscard]] Health health(MonoNanos now) const;

    // The catalogue of one gateway, or of all of them when `account` is empty.
    [[nodiscard]] std::vector<Instrument> instruments(std::string_view account,
                                                      std::optional<MarketType> market) const;

    [[nodiscard]] std::optional<Instrument> instrument(InstrumentId id) const;

    [[nodiscard]] const Gateway* find(std::string_view account) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept { return gateways_.size(); }

private:
    std::vector<Gateway> gateways_;
};

// Exposed for tests and for anything that needs one row without a segment.
[[nodiscard]] Instrument describe(const raikiri::Instrument& instrument, std::string_view account);

} // namespace raikiri::admin
