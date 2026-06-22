#include <raikiri/venues/htx/book.hpp>
#include <raikiri/venues/htx/envelope.hpp>
#include <raikiri/wire/json.hpp>

#include <optional>

namespace raikiri::venues::htx {

namespace detail = raikiri::wire;

namespace {

using detail::guarded;
using detail::session;

// A level and a top-of-book quote are both [price, size], with both members
// written as JSON numbers.
// Every price and size goes through raw_of: HTX writes them as numbers, and
// "1.00000000E-10" is a spelling only the token preserves.
std::expected<void, WireError> read_pair(detail::ondemand::value entry,
                                         std::string_view& price,
                                         std::string_view& qty,
                                         const detail::Session& s) {
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
            const auto raw = detail::raw_of(value, s);
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

std::expected<std::size_t, WireError> read_levels(detail::ondemand::value side,
                                                  std::span<WireLevel> into) {
    detail::ondemand::array array;
    if (const auto ec = side.get_array().get(array); ec != simdjson::SUCCESS) {
        return std::unexpected(detail::map_error(ec));
    }
    const auto& s = session();
    std::size_t count = 0;
    for (auto entry : array) {
        if (count == k_max_levels) {
            return std::unexpected(WireError::TooManyEntries);
        }
        detail::ondemand::value value;
        if (const auto ec = entry.get(value); ec != simdjson::SUCCESS) {
            return std::unexpected(detail::map_error(ec));
        }
        if (const auto ok = read_pair(value, into[count].price, into[count].qty, s);
            !ok.has_value()) {
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
        bool bid = false;
        bool ask = false;
        std::expected<std::uint64_t, WireError> ts{0};
        std::expected<std::uint64_t, WireError> version{0};
        const auto walked = detail::for_each_field(
            *tick,
            [&](std::string_view key,
                detail::ondemand::value& value) -> std::expected<void, WireError> {
                if (key == "bid" || key == "ask") {
                    const bool is_bid = key == "bid";
                    (is_bid ? bid : ask) = true;
                    return is_bid ? read_pair(value, out.bid_price, out.bid_qty, session())
                                  : read_pair(value, out.ask_price, out.ask_qty, session());
                }
                if (key == "ts") {
                    ts = detail::u64_of(value);
                } else if (key == "version") {
                    version = detail::u64_of(value);
                }
                return {};
            });
        if (!walked.has_value()) {
            return std::unexpected(walked.error());
        }
        if (!bid || !ask) {
            return std::unexpected(WireError::MissingField);
        }
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
        std::expected<std::uint64_t, WireError> mrid{0};
        std::expected<std::uint64_t, WireError> ts{0};
        std::expected<std::uint64_t, WireError> version{0};
        std::optional<std::size_t> bids;
        std::optional<std::size_t> asks;
        const auto walked = detail::for_each_field(
            *tick,
            [&](std::string_view key,
                detail::ondemand::value& value) -> std::expected<void, WireError> {
                if (key == "bids" || key == "asks") {
                    const bool bid = key == "bids";
                    const auto count = read_levels(value, bid ? storage.bids : storage.asks);
                    if (!count.has_value()) {
                        return std::unexpected(count.error());
                    }
                    (bid ? bids : asks) = *count;
                } else if (key == "mrid") {
                    mrid = detail::u64_of(value);
                } else if (key == "ts") {
                    ts = detail::u64_of(value);
                } else if (key == "version") {
                    version = detail::u64_of(value);
                }
                return {};
            });
        if (!walked.has_value()) {
            return std::unexpected(walked.error());
        }
        if (!mrid.has_value()) {
            return std::unexpected(mrid.error());
        }
        if (!bids.has_value() || !asks.has_value()) {
            return std::unexpected(WireError::MissingField);
        }
        if (!ts.has_value() || !version.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        out.mrid = *mrid;
        out.ts_ms = *ts;
        out.version = *version;
        out.bids = {storage.bids.data(), *bids};
        out.asks = {storage.asks.data(), *asks};
        return out;
    });
}

} // namespace raikiri::venues::htx
