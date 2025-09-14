#include <raikiri/venues/xt/book.hpp>
#include <raikiri/venues/xt/envelope.hpp>

namespace raikiri::venues::xt {

// The shared JSON readers. `raikiri::wire` is the deliberate simdjson
// layer; a venue that reimplements opt_str or req_array is one that will
// disagree with its neighbours about what an absent field means.
namespace detail = raikiri::wire;

namespace {

using detail::guarded;
using detail::session;

// A level is ["64152.1","71044"]: price and a count of contracts. XT appends
// nothing after those two, unlike okx's four-element form.
std::expected<WireLevel, WireError> read_level(detail::ondemand::value entry) {
    detail::ondemand::array pair;
    if (const auto ec = entry.get_array().get(pair); ec != simdjson::SUCCESS) {
        return std::unexpected(detail::map_error(ec));
    }
    WireLevel level{};
    std::size_t field = 0;
    for (auto element : pair) {
        detail::ondemand::value value;
        if (const auto ec = element.get(value); ec != simdjson::SUCCESS) {
            return std::unexpected(detail::map_error(ec));
        }
        if (field < 2) {
            const auto text = detail::str_of(value);
            if (!text.has_value()) {
                return std::unexpected(text.error());
            }
            (field == 0 ? level.price : level.qty) = *text;
        }
        ++field;
    }
    if (field < 2) {
        return std::unexpected(WireError::MissingField);
    }
    return level;
}

std::expected<std::size_t, WireError>
read_levels(detail::ondemand::object& obj, std::string_view key, std::span<WireLevel> into) {
    auto array = detail::req_array(obj, key);
    if (!array.has_value()) {
        return std::unexpected(array.error());
    }
    std::size_t count = 0;
    for (auto entry : *array) {
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

std::expected<WireDepth, WireError>
parse_depth(std::string_view frame, LevelBuffer& storage, MarketType market) {
    const bool spot = market == MarketType::Spot;
    return guarded([&]() -> std::expected<WireDepth, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        return with_data(
            *root, [&](detail::ondemand::object& data) -> std::expected<WireDepth, WireError> {
                WireDepth out{};
                const auto symbol = detail::req_str(data, "s");
                if (!symbol.has_value()) {
                    return std::unexpected(symbol.error());
                }
                out.symbol = *symbol;
                // Futures stamp the snapshot as digit-string "id"; spot as a
                // JSON number "i". Reading only the futures spelling refused
                // every spot snapshot, and the refusal was silent: the socket
                // stayed up and the book simply never appeared.
                const auto update_id = spot ? detail::opt_u64(data, "i") : req_digits(data, "id");
                if (!update_id.has_value()) {
                    return std::unexpected(update_id.error());
                }
                out.update_id = *update_id;
                const auto bids = read_levels(data, "b", storage.bids);
                if (!bids.has_value()) {
                    return std::unexpected(bids.error());
                }
                const auto asks = read_levels(data, "a", storage.asks);
                if (!asks.has_value()) {
                    return std::unexpected(asks.error());
                }
                const auto ts = opt_ms(data, "t");
                if (!ts.has_value()) {
                    return std::unexpected(ts.error());
                }
                out.ts_ms = *ts;
                out.bids = {storage.bids.data(), *bids};
                out.asks = {storage.asks.data(), *asks};
                return out;
            });
    });
}

std::expected<WireDepthUpdate, WireError> parse_depth_update(std::string_view frame,
                                                             LevelBuffer& storage) {
    return guarded([&]() -> std::expected<WireDepthUpdate, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        return with_data(
            *root,
            [&](detail::ondemand::object& data) -> std::expected<WireDepthUpdate, WireError> {
                WireDepthUpdate out{};
                const auto symbol = detail::req_str(data, "s");
                const auto first = req_digits(data, "fu");
                const auto prev = req_digits(data, "pu");
                const auto update_id = req_digits(data, "u");
                if (!symbol.has_value() || !first.has_value() || !prev.has_value() ||
                    !update_id.has_value()) {
                    return std::unexpected(WireError::WrongType);
                }
                out.symbol = *symbol;
                out.first_update_id = *first;
                out.prev_update_id = *prev;
                out.update_id = *update_id;
                const auto bids = read_levels(data, "b", storage.bids);
                if (!bids.has_value()) {
                    return std::unexpected(bids.error());
                }
                const auto asks = read_levels(data, "a", storage.asks);
                if (!asks.has_value()) {
                    return std::unexpected(asks.error());
                }
                const auto ts = opt_ms(data, "t");
                if (!ts.has_value()) {
                    return std::unexpected(ts.error());
                }
                out.ts_ms = *ts;
                out.bids = {storage.bids.data(), *bids};
                out.asks = {storage.asks.data(), *asks};
                return out;
            });
    });
}

} // namespace raikiri::venues::xt
