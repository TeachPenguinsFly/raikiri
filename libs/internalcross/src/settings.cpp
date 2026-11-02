#include <raikiri/internalcross/domain/settings.hpp>

#include <algorithm>
#include <format>

namespace raikiri::internalcross {

namespace internal {

constexpr std::int64_t k_max_fee_bp = 100;
constexpr std::int64_t k_max_spread_bp = 1000;
constexpr std::uint64_t k_max_age_ms = 60'000;
constexpr std::uint64_t k_max_hold_ms = 1'000;

[[nodiscard]] std::string_view market_word(MarketType market) noexcept {
    switch (market) {
    case MarketType::Spot:
        return "spot";
    case MarketType::Perp:
        return "perp";
    case MarketType::Future:
        return "future";
    case MarketType::Option:
        return "option";
    }
    return "?";
}

} // namespace internal

std::string validate(const Settings& settings) {
    if (settings.max_spread_bp <= 0 || settings.max_spread_bp > internal::k_max_spread_bp) {
        return std::format("max_spread_bp must be 1 to {}", internal::k_max_spread_bp);
    }
    if (settings.max_quote_age_ms == 0 || settings.max_quote_age_ms > internal::k_max_age_ms) {
        return std::format("max_quote_age_ms must be 1 to {}", internal::k_max_age_ms);
    }
    if (settings.hold_ms > internal::k_max_hold_ms) {
        return std::format("hold_ms must be at most {}", internal::k_max_hold_ms);
    }
    for (const auto& [name, policy] : settings.books) {
        if (!name.starts_with("client:")) {
            return std::format("{} is not a client book", name);
        }
        if (policy.fee_bp < 0 || policy.fee_bp > internal::k_max_fee_bp) {
            return std::format("{} fee_bp must be 0 to {}", name, internal::k_max_fee_bp);
        }
        for (const auto& entry : policy.instruments) {
            const auto colon = entry.rfind(':');
            const auto market = colon == std::string::npos ? std::string{} : entry.substr(colon + 1);
            if (colon == 0 || (market != "spot" && market != "perp" && market != "future" && market != "option")) {
                return std::format("{} instrument {} must read <symbol>:<spot|perp|future|option>", name, entry);
            }
        }
    }
    return {};
}

bool allows(const BookPolicy& policy, std::string_view local_symbol, MarketType market) noexcept {
    const auto word = internal::market_word(market);
    return std::ranges::any_of(policy.instruments, [&](const std::string& entry) {
        const std::string_view text{entry};
        return text.size() == local_symbol.size() + 1 + word.size() && text.starts_with(local_symbol) &&
               text[local_symbol.size()] == ':' && text.ends_with(word);
    });
}

std::vector<std::string> raises(const Settings& before, const Settings& after) {
    std::vector<std::string> out;
    if (after.enabled && !before.enabled) {
        out.emplace_back("turns internal crossing on");
    }
    if (after.against_desk && !before.against_desk) {
        out.emplace_back("crosses clients against the hedge book");
    }
    if (after.max_spread_bp > before.max_spread_bp) {
        out.emplace_back("crosses on a wider book");
    }
    if (after.max_quote_age_ms > before.max_quote_age_ms) {
        out.emplace_back("crosses on an older quote");
    }
    if (after.hold_ms > before.hold_ms) {
        out.emplace_back("holds arriving orders longer");
    }
    for (const auto& [name, policy] : after.books) {
        const auto was = before.books.find(name);
        const bool on_before = was != before.books.end() && was->second.enabled;
        if (policy.enabled && !on_before) {
            out.push_back(name + " starts crossing");
        }
        for (const auto& entry : policy.instruments) {
            if (policy.enabled && on_before &&
                std::ranges::find(was->second.instruments, entry) == was->second.instruments.end()) {
                out.push_back(name + " may cross " + entry);
            }
        }
    }
    return out;
}

} // namespace raikiri::internalcross
