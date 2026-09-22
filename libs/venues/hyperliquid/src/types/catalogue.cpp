#include <raikiri/venues/hyperliquid/catalogue.hpp>
#include <raikiri/wire/json.hpp>

#include <algorithm>
#include <array>

namespace raikiri::venues::hyperliquid {

namespace {

using wire::guarded;
using wire::session;

std::expected<std::size_t, WireError> read_universe(wire::ondemand::object& meta,
                                                    AssetBuffer& storage) {
    auto array = wire::req_array(meta, "universe");
    if (!array.has_value()) {
        return std::unexpected(array.error());
    }
    std::size_t count = 0;
    for (auto entry : *array) {
        if (count == k_max_assets) {
            return std::unexpected(WireError::TooManyEntries);
        }
        wire::ondemand::object obj;
        if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
            return std::unexpected(wire::map_error(ec));
        }
        WireAsset out{};
        const auto name = wire::req_str(obj, "name");
        const auto sz_decimals = wire::opt_i64(obj, "szDecimals");
        const auto max_leverage = wire::opt_i64(obj, "maxLeverage");
        const auto delisted = wire::opt_bool(obj, "isDelisted");
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
        const auto collateral = wire::opt_i64(*root, "collateralToken");
        if (!collateral.has_value()) {
            return std::unexpected(collateral.error());
        }
        storage.collateral_token = *collateral;
        return std::span<const WireAsset>{storage.assets.data(), *count};
    });
}

std::expected<std::span<const WireCategory>, WireError>
parse_perp_categories(std::string_view body, PerpCategoryBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireCategory>, WireError> {
        auto rows = session().open_array(body);
        if (!rows.has_value()) {
            return std::unexpected(rows.error());
        }
        std::size_t count = 0;
        std::size_t used = 0;
        for (auto row : *rows) {
            if (count == k_max_perp_categories) {
                return std::unexpected(WireError::TooManyEntries);
            }
            wire::ondemand::array pair;
            if (const auto ec = row.get_array().get(pair); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
            }
            std::array<std::string_view, 2> fields{};
            std::size_t read = 0;
            for (auto field : pair) {
                if (read == fields.size()) {
                    return std::unexpected(WireError::TooManyEntries);
                }
                if (const auto ec = field.get_string().get(fields[read]); ec != simdjson::SUCCESS) {
                    return std::unexpected(wire::map_error(ec));
                }
                ++read;
            }
            if (read != fields.size()) {
                return std::unexpected(WireError::MissingField);
            }
            std::array<std::string_view, 2> kept{};
            for (std::size_t i = 0; i < fields.size(); ++i) {
                if (used + fields[i].size() > storage.text.size()) {
                    return std::unexpected(WireError::TooManyEntries);
                }
                std::ranges::copy(fields[i], storage.text.begin() + static_cast<std::ptrdiff_t>(used));
                kept[i] = {storage.text.data() + used, fields[i].size()};
                used += fields[i].size();
            }
            storage.rows[count++] = {.name = kept[0], .category = kept[1]};
        }
        return std::span<const WireCategory>{storage.rows.data(), count};
    });
}

std::expected<std::span<const std::string_view>, WireError>
parse_perp_dexs(std::string_view body, PerpDexBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const std::string_view>, WireError> {
        auto rows = session().open_array(body);
        if (!rows.has_value()) {
            return std::unexpected(rows.error());
        }
        std::size_t count = 0;
        for (auto row : *rows) {
            bool null_row = false;
            if (row.is_null().get(null_row) == simdjson::SUCCESS && null_row) {
                continue;
            }
            if (count == k_max_perp_dexs) {
                return std::unexpected(WireError::TooManyEntries);
            }
            wire::ondemand::object obj;
            if (const auto ec = row.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
            }
            const auto name = wire::req_str(obj, "name");
            if (!name.has_value()) {
                return std::unexpected(name.error());
            }
            storage.names[count++] = *name;
        }
        return std::span<const std::string_view>{storage.names.data(), count};
    });
}

std::expected<std::string_view, WireError> parse_token_name(std::string_view body,
                                                            std::int64_t index) {
    return guarded([&]() -> std::expected<std::string_view, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto rows = wire::req_array(*root, "tokens");
        if (!rows.has_value()) {
            return std::unexpected(rows.error());
        }
        for (auto row : *rows) {
            wire::ondemand::object obj;
            if (const auto ec = row.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
            }
            const auto name = wire::req_str(obj, "name");
            const auto at = wire::opt_i64(obj, "index");
            if (!name.has_value() || !at.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            if (*at == index) {
                return *name;
            }
        }
        return std::unexpected(WireError::MissingField);
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

        auto token_rows = wire::req_array(*root, "tokens");
        if (!token_rows.has_value()) {
            return std::unexpected(token_rows.error());
        }
        for (auto row : *token_rows) {
            wire::ondemand::object obj;
            if (const auto ec = row.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
            }
            const auto name = wire::req_str(obj, "name");
            const auto decimals = wire::opt_i64(obj, "szDecimals");
            const auto index = wire::opt_i64(obj, "index");
            if (!name.has_value() || !decimals.has_value() || !index.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            if (*index < 0 || static_cast<std::size_t>(*index) >= tokens.size()) {
                return std::unexpected(WireError::TooManyEntries);
            }
            tokens[static_cast<std::size_t>(*index)] = {
                .name = *name, .sz_decimals = *decimals, .present = true};
        }

        auto pairs = wire::req_array(*root, "universe");
        if (!pairs.has_value()) {
            return std::unexpected(pairs.error());
        }
        std::size_t count = 0;
        for (auto row : *pairs) {
            if (count == k_max_spot_assets) {
                return std::unexpected(WireError::TooManyEntries);
            }
            wire::ondemand::object obj;
            if (const auto ec = row.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
            }
            const auto name = wire::req_str(obj, "name");
            const auto index = wire::opt_i64(obj, "index");
            if (!name.has_value() || !index.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            auto members = wire::req_array(obj, "tokens");
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
                    return std::unexpected(wire::map_error(ec));
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
