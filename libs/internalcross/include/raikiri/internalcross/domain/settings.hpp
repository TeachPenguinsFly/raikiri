#pragma once

// what people decided about crossing: off until a person says otherwise, book by book and instrument by instrument

#include <raikiri/domain/identifiers.hpp>
#include <raikiri/domain/instrument.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace raikiri::internalcross {

struct BookPolicy {
    bool enabled{false};
    // charged on an internal fill, per side; default zero
    std::int64_t fee_bp{0};
    // "<local symbol>:<market>", as "BTC/USDT:perp"; an empty list allows nothing
    std::vector<std::string> instruments{};

    friend bool operator==(const BookPolicy&, const BookPolicy&) = default;
};

struct Settings {
    bool enabled{false};
    std::int64_t max_spread_bp{20};
    std::uint64_t max_quote_age_ms{1000};
    // how long an arriving order waits for an opposite one; zero is not at all
    std::uint64_t hold_ms{0};
    // clients may also trade against the hedge book. off on its own: while the rfq desk hedges into that book, its
    // position is not inventory but the other side of liabilities kept outside strategyd, and selling it unhedges them
    bool against_desk{false};
    // by book name, since a risk file may name a book before it has a number
    std::map<std::string, BookPolicy> books{};

    friend bool operator==(const Settings&, const Settings&) = default;
};

// empty when fine; stays well under the 10 s an order may sit unknown to the venue before reconciliation closes it
[[nodiscard]] std::string validate(const Settings& settings);

[[nodiscard]] bool allows(const BookPolicy& policy, std::string_view local_symbol, MarketType market) noexcept;

// what the change lets through that was not before, for the person to confirm
[[nodiscard]] std::vector<std::string> raises(const Settings& before, const Settings& after);

} // namespace raikiri::internalcross
