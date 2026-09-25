#include <raikiri/venues/hyperliquid/book.hpp>
#include <raikiri/venues/hyperliquid/envelope.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::hyperliquid {

namespace {

using wire::guarded;
using wire::session;

std::expected<WireLevel, WireError> read_level(wire::ondemand::value entry) {
    wire::ondemand::object obj;
    if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
        return std::unexpected(wire::map_error(ec));
    }
    WireLevel level{};
    const auto price = wire::req_str(obj, "px");
    const auto qty = wire::req_str(obj, "sz");
    if (!price.has_value() || !qty.has_value()) {
        return std::unexpected(WireError::MissingField);
    }
    level.price = *price;
    level.qty = *qty;
    return level;
}

std::expected<std::size_t, WireError> read_levels(wire::ondemand::array levels, std::span<WireLevel> into) {
    std::size_t count = 0;
    for (auto entry : levels) {
        if (count == k_max_levels) {
            return std::unexpected(WireError::TooManyEntries);
        }
        wire::ondemand::value value;
        if (const auto ec = entry.get(value); ec != simdjson::SUCCESS) {
            return std::unexpected(wire::map_error(ec));
        }
        const auto level = read_level(value);
        if (!level.has_value()) {
            return std::unexpected(level.error());
        }
        into[count] = *level;
        ++count;
    }
    return count;
}

} // namespace

std::expected<WireBbo, WireError> parse_bbo(std::string_view frame) {
    return guarded([&]() -> std::expected<WireBbo, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        return with_data_object(*root, [](wire::ondemand::object& obj) -> std::expected<WireBbo, WireError> {
            WireBbo out{};
            const auto coin = wire::req_str(obj, "coin");
            const auto ts = opt_number_ms(obj, "time");
            if (!coin.has_value() || !ts.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            out.coin = *coin;
            out.ts_ms = *ts;
            auto pair = wire::req_array(obj, "bbo");
            if (!pair.has_value()) {
                return std::unexpected(pair.error());
            }
            std::size_t index = 0;
            for (auto entry : *pair) {
                wire::ondemand::value value;
                if (const auto ec = entry.get(value); ec != simdjson::SUCCESS) {
                    return std::unexpected(wire::map_error(ec));
                }
                if (value.is_null()) {
                    ++index;
                    continue;
                }
                const auto level = read_level(value);
                if (!level.has_value()) {
                    return std::unexpected(level.error());
                }
                if (index == 0) {
                    out.bid_price = level->price;
                    out.bid_qty = level->qty;
                } else if (index == 1) {
                    out.ask_price = level->price;
                    out.ask_qty = level->qty;
                }
                ++index;
            }
            if (index != 2) {
                return std::unexpected(WireError::MissingField);
            }
            return out;
        });
    });
}

std::expected<WireBook, WireError> parse_book(std::string_view frame, LevelBuffer& storage) {
    return guarded([&]() -> std::expected<WireBook, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        return with_data_object(*root, [&](wire::ondemand::object& obj) -> std::expected<WireBook, WireError> {
            WireBook out{};
            const auto coin = wire::req_str(obj, "coin");
            const auto ts = opt_number_ms(obj, "time");
            if (!coin.has_value() || !ts.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            out.coin = *coin;
            out.ts_ms = *ts;
            auto levels = wire::req_array(obj, "levels");
            if (!levels.has_value()) {
                return std::unexpected(levels.error());
            }
            std::size_t side = 0;
            for (auto entry : *levels) {
                wire::ondemand::array inner;
                if (const auto ec = entry.get_array().get(inner); ec != simdjson::SUCCESS) {
                    return std::unexpected(wire::map_error(ec));
                }
                if (side > 1) {
                    return std::unexpected(WireError::TooManyEntries);
                }
                auto& into = side == 0 ? storage.bids : storage.asks;
                const auto count = read_levels(inner, into);
                if (!count.has_value()) {
                    return std::unexpected(count.error());
                }
                // assigned by name; libstdc++ rejects a brace list here
                (side == 0 ? out.bids : out.asks) = std::span<const WireLevel>{into.data(), *count};
                ++side;
            }
            if (side != 2) {
                return std::unexpected(WireError::MissingField);
            }
            return out;
        });
    });
}

} // namespace raikiri::venues::hyperliquid
