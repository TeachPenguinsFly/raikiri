#include <raikiri/venues/htx/book.hpp>
#include <raikiri/venues/htx/envelope.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::htx {

namespace detail = raikiri::wire;

namespace {

using detail::guarded;
using detail::session;

// The raw JSON text of a value, whatever its type, quotes included. Every
// price and size on the ladder goes through this: HTX writes them as numbers,
// and "1.00000000E-10" is a spelling only the token preserves.
//
// raw_json() on a scalar returns the token plus whitespace up to the next one,
// so the trim is what makes the token exact. Contents stay escaped: HTX
// contract codes and decimal tokens cannot contain an escape, and letting
// simdjson unescape would move every field into a buffer that cannot be
// rebased.
std::expected<std::string_view, WireError> raw_of(detail::ondemand::value value) {
    std::string_view raw;
    if (const auto ec = value.raw_json().get(raw); ec != simdjson::SUCCESS) {
        return std::unexpected(detail::map_error(ec));
    }
    return session().rebase(detail::trim(raw));
}

// A level and a top-of-book quote are both [price, size], with both members
// written as JSON numbers.
std::expected<void, WireError>
read_pair(detail::ondemand::value entry, std::string_view& price, std::string_view& qty) {
    detail::ondemand::array pair;
    if (const auto ec = entry.get_array().get(pair); ec != simdjson::SUCCESS) {
        return std::unexpected(detail::map_error(ec));
    }
    std::size_t field = 0;
    for (auto element : pair) {
        detail::ondemand::value value;
        if (const auto ec = element.get(value); ec != simdjson::SUCCESS) {
            return std::unexpected(detail::map_error(ec));
        }
        if (field < 2) {
            const auto raw = raw_of(value);
            if (!raw.has_value()) {
                return std::unexpected(raw.error());
            }
            (field == 0 ? price : qty) = *raw;
        }
        ++field;
    }
    if (field < 2) {
        return std::unexpected(WireError::MissingField);
    }
    return {};
}

std::expected<void, WireError> read_quote(detail::ondemand::object& tick,
                                          std::string_view key,
                                          std::string_view& price,
                                          std::string_view& qty) {
    detail::ondemand::value value;
    if (const auto ec = tick.find_field_unordered(key).get(value); ec != simdjson::SUCCESS) {
        return std::unexpected(detail::map_error(ec));
    }
    return read_pair(value, price, qty);
}

std::expected<std::size_t, WireError>
read_levels(detail::ondemand::object& tick, std::string_view key, std::span<WireLevel> into) {
    auto array = detail::req_array(tick, key);
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
        if (const auto ok = read_pair(value, into[count].price, into[count].qty); !ok.has_value()) {
            return std::unexpected(ok.error());
        }
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
        const auto ch = detail::req_str(*root, "ch");
        if (!ch.has_value()) {
            return std::unexpected(ch.error());
        }
        auto tick = detail::req_object(*root, "tick");
        if (!tick.has_value()) {
            return std::unexpected(tick.error());
        }
        WireBbo out{};
        out.contract_code = code_of(*ch);
        if (const auto ok = read_quote(*tick, "bid", out.bid_price, out.bid_qty); !ok.has_value()) {
            return std::unexpected(ok.error());
        }
        if (const auto ok = read_quote(*tick, "ask", out.ask_price, out.ask_qty); !ok.has_value()) {
            return std::unexpected(ok.error());
        }
        const auto ts = detail::opt_u64(*tick, "ts");
        const auto version = detail::opt_u64(*tick, "version");
        if (!ts.has_value() || !version.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        out.ts_ms = *ts;
        out.version = *version;
        return out;
    });
}

std::expected<WireDepth, WireError> parse_depth(std::string_view frame, LevelBuffer& storage) {
    return guarded([&]() -> std::expected<WireDepth, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto ch = detail::req_str(*root, "ch");
        if (!ch.has_value()) {
            return std::unexpected(ch.error());
        }
        auto tick = detail::req_object(*root, "tick");
        if (!tick.has_value()) {
            return std::unexpected(tick.error());
        }
        WireDepth out{};
        out.contract_code = code_of(*ch);
        const auto mrid = detail::opt_u64(*tick, "mrid");
        if (!mrid.has_value()) {
            return std::unexpected(mrid.error());
        }
        out.mrid = *mrid;
        const auto bids = read_levels(*tick, "bids", storage.bids);
        if (!bids.has_value()) {
            return std::unexpected(bids.error());
        }
        const auto asks = read_levels(*tick, "asks", storage.asks);
        if (!asks.has_value()) {
            return std::unexpected(asks.error());
        }
        const auto ts = detail::opt_u64(*tick, "ts");
        const auto version = detail::opt_u64(*tick, "version");
        if (!ts.has_value() || !version.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        out.ts_ms = *ts;
        out.version = *version;
        out.bids = {storage.bids.data(), *bids};
        out.asks = {storage.asks.data(), *asks};
        return out;
    });
}

} // namespace raikiri::venues::htx
