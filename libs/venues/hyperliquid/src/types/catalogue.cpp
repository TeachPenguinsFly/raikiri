#include <raikiri/venues/hyperliquid/catalogue.hpp>
#include <raikiri/wire/json.hpp>

#include <array>

namespace raikiri::venues::hyperliquid {

// The shared JSON readers. `raikiri::wire` is the deliberate simdjson
// layer; a venue that reimplements opt_str or req_array is one that will
// disagree with its neighbours about what an absent field means.
namespace detail = raikiri::wire;

namespace {

using detail::guarded;
using detail::session;

std::expected<std::size_t, WireError> read_universe(detail::ondemand::object& meta,
                                                    AssetBuffer& storage) {
    auto array = detail::req_array(meta, "universe");
    if (!array.has_value()) {
        return std::unexpected(array.error());
    }
    std::size_t count = 0;
    for (auto entry : *array) {
        if (count == k_max_assets) {
            return std::unexpected(WireError::TooManyEntries);
        }
        detail::ondemand::object obj;
        if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
            return std::unexpected(detail::map_error(ec));
        }
        WireAsset out{};
        const auto name = detail::req_str(obj, "name");
        const auto sz_decimals = detail::opt_i64(obj, "szDecimals");
        const auto max_leverage = detail::opt_i64(obj, "maxLeverage");
        const auto delisted = detail::opt_bool(obj, "isDelisted");
        if (!name.has_value() || !sz_decimals.has_value() || !max_leverage.has_value() ||
            !delisted.has_value()) {
            return std::unexpected(WireError::MissingField);
        }
        out.name = *name;
        out.sz_decimals = *sz_decimals;
        out.max_leverage = *max_leverage;
        out.index = static_cast<std::int64_t>(count);
        out.delisted = *delisted;
        storage.assets[count] = out;
        ++count;
    }
    return count;
}

} // namespace

std::expected<std::span<const WireAsset>, WireError> parse_meta(std::string_view body,
                                                                AssetBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireAsset>, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto count = read_universe(*root, storage);
        if (!count.has_value()) {
            return std::unexpected(count.error());
        }
        return std::span<const WireAsset>{storage.assets.data(), *count};
    });
}

std::expected<std::span<const WireSpotAsset>, WireError> parse_spot_meta(std::string_view body,
                                                                         SpotAssetBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireSpotAsset>, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }

        // Token index -> (name, szDecimals). Indices are dense and small today
        // but the join is by VALUE, not by position: a sparse tokens array
        // would otherwise attach one token's precision to another's pair.
        struct Token {
            std::string_view name;
            std::int64_t sz_decimals{0};
            bool present{false};
        };
        static thread_local std::array<Token, k_max_spot_assets> tokens{};
        tokens.fill(Token{});

        auto token_rows = detail::req_array(*root, "tokens");
        if (!token_rows.has_value()) {
            return std::unexpected(token_rows.error());
        }
        for (auto row : *token_rows) {
            detail::ondemand::object obj;
            if (const auto ec = row.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(detail::map_error(ec));
            }
            const auto name = detail::req_str(obj, "name");
            const auto decimals = detail::opt_i64(obj, "szDecimals");
            const auto index = detail::opt_i64(obj, "index");
            if (!name.has_value() || !decimals.has_value() || !index.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            if (*index < 0 || static_cast<std::size_t>(*index) >= tokens.size()) {
                return std::unexpected(WireError::TooManyEntries);
            }
            tokens[static_cast<std::size_t>(*index)] = {
                .name = *name, .sz_decimals = *decimals, .present = true};
        }

        auto pairs = detail::req_array(*root, "universe");
        if (!pairs.has_value()) {
            return std::unexpected(pairs.error());
        }
        std::size_t count = 0;
        for (auto row : *pairs) {
            if (count == k_max_spot_assets) {
                return std::unexpected(WireError::TooManyEntries);
            }
            detail::ondemand::object obj;
            if (const auto ec = row.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(detail::map_error(ec));
            }
            const auto name = detail::req_str(obj, "name");
            const auto index = detail::opt_i64(obj, "index");
            if (!name.has_value() || !index.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            auto members = detail::req_array(obj, "tokens");
            if (!members.has_value()) {
                return std::unexpected(members.error());
            }
            // [base, quote], and BOTH are read: the quote is NOT always USDC.
            // Seventeen pairs settle in USDH, USDT0 or USDE, twelve of those
            // bases also list against USDC, and a name taken from the first
            // member alone calls four separate HYPE books "HYPE/USDC".
            std::array<std::int64_t, 2> members_at{};
            std::size_t read = 0;
            for (auto member : *members) {
                if (read == members_at.size()) {
                    return std::unexpected(WireError::TooManyEntries);
                }
                if (const auto ec = member.get_int64().get(members_at[read]);
                    ec != simdjson::SUCCESS) {
                    return std::unexpected(detail::map_error(ec));
                }
                ++read;
            }
            // A row that is not exactly two tokens is a pair model this adapter
            // cannot name, not one row to skip past.
            if (read != members_at.size()) {
                return std::unexpected(WireError::MissingField);
            }
            const auto resolve = [&](std::int64_t at) -> const Token* {
                if (at < 0 || static_cast<std::size_t>(at) >= tokens.size()) {
                    return nullptr;
                }
                const auto& token = tokens[static_cast<std::size_t>(at)];
                return token.present ? &token : nullptr;
            };
            const auto* base = resolve(members_at[0]);
            const auto* quote = resolve(members_at[1]);
            if (base == nullptr || quote == nullptr) {
                continue; // a pair naming a token this body did not carry
            }
            // Both steps derive from the BASE token's precision; the quote is
            // carried for the name and nothing else.
            storage.assets[count] = {.name = *name,
                                     .base = base->name,
                                     .quote = quote->name,
                                     .index = *index,
                                     .sz_decimals = base->sz_decimals};
            ++count;
        }
        return std::span<const WireSpotAsset>{storage.assets.data(), count};
    });
}

} // namespace raikiri::venues::hyperliquid
