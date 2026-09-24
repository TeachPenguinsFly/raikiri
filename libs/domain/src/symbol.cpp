#include <raikiri/domain/symbol.hpp>

#include <array>
#include <cstddef>
#include <string>

namespace raikiri {

namespace {

// two spellings of one asset; keep it tiny and never fold a scale (1000PEPE is not PEPE)
struct Alias {
    std::string_view from;
    std::string_view to;
};

constexpr std::array<Alias, 1> k_aliases{{
    {"XBT", "BTC"}, // kraken, bitmex and kucoin spell bitcoin the ISO way
}};

[[nodiscard]] AssetCode aliased(AssetCode code) noexcept {
    for (const auto& alias : k_aliases) {
        if (code.view() == alias.from) {
            if (const auto to = AssetCode::from(alias.to); to.has_value()) {
                return *to;
            }
        }
    }
    return code;
}

} // namespace

std::optional<SymbolText> canonical_symbol(std::string_view base, std::string_view quote) noexcept {
    const auto base_code = to_asset_code(base);
    const auto quote_code = to_asset_code(quote);
    if (!base_code.has_value() || !quote_code.has_value()) {
        return std::nullopt;
    }
    const auto left = aliased(*base_code);
    const auto right = aliased(*quote_code);

    // the separator keeps BTC+USDT apart from BTCU+SDT
    std::string out;
    out.reserve(left.view().size() + 1 + right.view().size());
    out.append(left.view());
    out.push_back('/');
    out.append(right.view());

    const auto symbol = SymbolText::from(out);
    return symbol.has_value() ? std::optional{*symbol} : std::nullopt;
}

std::optional<std::pair<AssetCode, AssetCode>> split_symbol(std::string_view symbol,
                                                            char separator) noexcept {
    const auto cut = symbol.find(separator);
    if (cut == std::string_view::npos) {
        return std::nullopt;
    }
    const auto base = symbol.substr(0, cut);
    const auto quote = symbol.substr(cut + 1);
    if (quote.find(separator) != std::string_view::npos) {
        return std::nullopt;
    }
    const auto base_code = to_asset_code(base);
    const auto quote_code = to_asset_code(quote);
    if (!base_code.has_value() || !quote_code.has_value()) {
        return std::nullopt;
    }
    return std::pair{aliased(*base_code), aliased(*quote_code)};
}

} // namespace raikiri
