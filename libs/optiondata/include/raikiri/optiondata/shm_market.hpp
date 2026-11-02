#pragma once

#include <raikiri/admin/registry.hpp>
#include <raikiri/optiondata/queries.hpp>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace raikiri::optiondata {

// the attached gateways' option listings, indexed by underlying and expiry; the index is rebuilt when the
// registry re-attaches or a catalogue grows, and a read touches only the cells of the asked expiries
class ShmOptionMarket final : public OptionMarket {
public:
    explicit ShmOptionMarket(const admin::Registry& registry) : registry_{&registry} {}

    [[nodiscard]] std::vector<UnderlyingListing> underlyings() const override;
    void
    read(std::string_view underlying, std::optional<UnixNanos> expiry, std::vector<OptionQuote>& out) const override;

private:
    struct Listed {
        const shm::GatewayChannels* channels{nullptr};
        const OptionInstrument* option{nullptr};
        options::OptionKey key{};
    };

    using Expiries = std::map<std::uint64_t, std::vector<Listed>>;

    void refresh() const;

    const admin::Registry* registry_;
    mutable std::uint64_t epoch_{~0ULL};
    mutable std::size_t gateways_{0};
    mutable std::uint64_t listed_{0};
    mutable std::map<std::string, Expiries, std::less<>> index_;
    mutable std::vector<UnderlyingListing> summary_;
};

} // namespace raikiri::optiondata
