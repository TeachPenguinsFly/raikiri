#include <raikiri/venues/hyperliquid/book.hpp>
#include <raikiri/venues/hyperliquid/envelope.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::hyperliquid {

// The shared JSON readers. `raikiri::wire` is the deliberate simdjson
// layer; a venue that reimplements opt_str or req_array is one that will
// disagree with its neighbours about what an absent field means.
namespace detail = raikiri::wire;

namespace {

using detail::guarded;
using detail::session;

// {"px":"64143.0","sz":"26.35385","n":66}; nothing downstream models `n`.
std::expected<WireLevel, WireError> read_level(detail::ondemand::value entry) {
    detail::ondemand::object obj;
    if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
        return std::unexpected(detail::map_error(ec));
    }
    WireLevel level{};
    const auto price = detail::req_str(obj, "px");
    const auto qty = detail::req_str(obj, "sz");
    if (!price.has_value() || !qty.has_value()) {
        return std::unexpected(WireError::MissingField);
    }
    level.price = *price;
    level.qty = *qty;
    return level;
}

std::expected<std::size_t, WireError> read_levels(detail::ondemand::array levels,
                                                  std::span<WireLevel> into) {
    std::size_t count = 0;
    for (auto entry : levels) {
        if (count == k_max_levels) {
            return std::unexpected(WireError::TooManyEntries);
        }
        detail::ondemand::value value;
        if (const auto ec = entry.get(value); ec != simdjson::SUCCESS) {
            return std::unexpected(detail::map_error(ec));
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
        return with_data_object(
            *root, [](detail::ondemand::object& obj) -> std::expected<WireBbo, WireError> {
                WireBbo out{};
                const auto coin = detail::req_str(obj, "coin");
                const auto ts = opt_number_ms(obj, "time");
                if (!coin.has_value() || !ts.has_value()) {
                    return std::unexpected(WireError::MissingField);
                }
                out.coin = *coin;
                out.ts_ms = *ts;
                auto pair = detail::req_array(obj, "bbo");
                if (!pair.has_value()) {
                    return std::unexpected(pair.error());
                }
                std::size_t index = 0;
                for (auto entry : *pair) {
                    detail::ondemand::value value;
                    if (const auto ec = entry.get(value); ec != simdjson::SUCCESS) {
                        return std::unexpected(detail::map_error(ec));
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
        return with_data_object(
            *root, [&](detail::ondemand::object& obj) -> std::expected<WireBook, WireError> {
                WireBook out{};
                const auto coin = detail::req_str(obj, "coin");
                const auto ts = opt_number_ms(obj, "time");
                if (!coin.has_value() || !ts.has_value()) {
                    return std::unexpected(WireError::MissingField);
                }
                out.coin = *coin;
                out.ts_ms = *ts;
                auto levels = detail::req_array(obj, "levels");
                if (!levels.has_value()) {
                    return std::unexpected(levels.error());
                }
                std::size_t side = 0;
                for (auto entry : *levels) {
                    detail::ondemand::array inner;
                    if (const auto ec = entry.get_array().get(inner); ec != simdjson::SUCCESS) {
                        return std::unexpected(detail::map_error(ec));
                    }
                    if (side > 1) {
                        return std::unexpected(WireError::TooManyEntries);
                    }
                    auto& into = side == 0 ? storage.bids : storage.asks;
                    const auto count = read_levels(inner, into);
                    if (!count.has_value()) {
                        return std::unexpected(count.error());
                    }
                    // NAMED, not braced. Assigning a brace list into a span
                    // works on a plain member and is rejected by libstdc++
                    // when the target is reached through a reference or a
                    // conditional, as it is here — measured both ways under
                    // gcc 14. The sibling adapters write to the member
                    // directly and are unaffected.
                    (side == 0 ? out.bids : out.asks) =
                        std::span<const WireLevel>{into.data(), *count};
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
