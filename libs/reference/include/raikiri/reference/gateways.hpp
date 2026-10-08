#pragma once

#include <raikiri/domain/identifiers.hpp>
#include <raikiri/domain/instrument.hpp>
#include <raikiri/reference/engine.hpp>
#include <raikiri/shm/records.hpp>
#include <raikiri/shm/segment.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace raikiri::reference {

// the touch size in the base asset; a linear contract counts contract_value coins, an inverse one contract_value usd
[[nodiscard]] FixedE9 base_qty(const InstrumentBase& base, QtyLots lots, FixedE9 price) noexcept;

// the book as the reference reads it; empty for anything but a spot or perp with an asset key
[[nodiscard]] std::optional<Quote>
quote_of(const Instrument& listed, const Bbo& bbo, const std::optional<TickerStats>& stats) noexcept;

// every gw.<account> segment under the directory, one reader slot each when one is free
class GatewayQuotes final : public QuoteSource {
public:
    GatewayQuotes(std::string shm_dir, std::string reader_name, std::function<std::uint64_t()> mono)
        : shm_dir_{std::move(shm_dir)}, reader_name_{std::move(reader_name)}, mono_{std::move(mono)} {}

    struct Rescan {
        std::vector<std::string> attached{};
        std::vector<std::string> unlisted{};
        std::vector<std::string> gone{};
    };

    // attaches new segments and lets go of ones whose file is gone or replaced
    Rescan rescan();

    [[nodiscard]] std::vector<AssetQuotes> snapshot() override;

    [[nodiscard]] std::size_t attached() const noexcept { return sources_.size(); }

    [[nodiscard]] std::size_t unlisted() const noexcept;

private:
    struct Source {
        std::string name{};
        shm::SegmentClient client;
        shm::GatewayChannels channels{};
        std::uint64_t file_id{0};
    };

    std::string shm_dir_;
    std::string reader_name_;
    std::function<std::uint64_t()> mono_;
    std::vector<std::unique_ptr<Source>> sources_;
};

} // namespace raikiri::reference
