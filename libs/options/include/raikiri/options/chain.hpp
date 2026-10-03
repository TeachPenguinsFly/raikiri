#pragma once

#include <raikiri/domain/instrument.hpp>
#include <raikiri/options/key.hpp>

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace raikiri::options {

struct ChainRow {
    FixedE9 strike{};
    std::optional<InstrumentId> call{};
    std::optional<InstrumentId> put{};
};

struct ChainExpiry {
    UnixNanos expiry{};
    std::vector<ChainRow> rows{}; // strike ascending
};

struct ChainUnderlying {
    UnderlyingText underlying{};
    std::vector<ChainExpiry> expiries{}; // nearest first
};

// a catalogue's options by underlying, expiry and strike; anything else is skipped
class OptionChain {
public:
    [[nodiscard]] static OptionChain build(std::span<const Instrument> catalogue);

    [[nodiscard]] std::span<const ChainUnderlying> underlyings() const noexcept { return underlyings_; }

    [[nodiscard]] const ChainUnderlying* underlying(std::string_view name) const noexcept;
    [[nodiscard]] const ChainExpiry* expiry(std::string_view underlying, UnixNanos expiry) const noexcept;
    [[nodiscard]] std::optional<InstrumentId> find(const OptionKey& key) const noexcept;

    // a second listing of one key is dropped
    [[nodiscard]] std::size_t size() const noexcept { return size_; }

private:
    std::vector<ChainUnderlying> underlyings_;
    std::size_t size_{0};
};

struct MatchedOption {
    OptionKey key{};
    InstrumentId left{};
    InstrumentId right{};
};

// the contracts both chains list, by exact expiry instant and strike, in chain order
[[nodiscard]] std::vector<MatchedOption> match(const OptionChain& left, const OptionChain& right);

} // namespace raikiri::options
