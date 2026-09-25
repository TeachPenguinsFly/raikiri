#include <raikiri/venues/extended/market_data.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::extended {

namespace json = raikiri::wire;

namespace {

struct Stamp {
    std::uint64_t seq{0};
    std::uint64_t ts_ms{0};
};

std::expected<Stamp, WireError> read_stamp(json::ondemand::object& root) {
    const auto ts = json::opt_u64(root, "ts");
    const auto seq = json::opt_u64(root, "seq");
    if (!ts.has_value() || !seq.has_value()) {
        return std::unexpected(WireError::WrongType);
    }
    if (*seq == 0) {
        return std::unexpected(WireError::MissingField);
    }
    return Stamp{.seq = *seq, .ts_ms = *ts};
}

std::expected<std::size_t, WireError>
read_levels(json::ondemand::object& data, std::string_view key, std::string_view size_key, std::span<WireLevel> into) {
    auto array = json::req_array(data, key);
    if (!array.has_value()) {
        return std::unexpected(array.error());
    }
    std::size_t count = 0;
    for (auto entry : *array) {
        if (count == into.size()) {
            return std::unexpected(WireError::TooManyEntries);
        }
        json::ondemand::object level;
        if (const auto ec = entry.get_object().get(level); ec != simdjson::SUCCESS) {
            return std::unexpected(json::map_error(ec));
        }
        const auto price = json::req_str(level, "p");
        const auto size = json::req_str(level, size_key);
        if (!price.has_value() || !size.has_value()) {
            return std::unexpected(WireError::MissingField);
        }
        into[count] = WireLevel{.price = *price, .size = *size};
        ++count;
    }
    return count;
}

} // namespace

std::expected<WireBook, WireError> parse_book(std::string_view frame, LevelBuffer& storage) {
    return json::guarded([&]() -> std::expected<WireBook, WireError> {
        auto root = json::session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        WireBook out{};
        const auto type = json::req_str(*root, "type");
        if (!type.has_value()) {
            return std::unexpected(type.error());
        }
        if (*type == "SNAPSHOT") {
            out.snapshot = true;
        } else if (*type != "DELTA") {
            return std::unexpected(WireError::UnknownMessage);
        }
        const std::string_view size_key = out.snapshot ? "q" : "c";
        {
            auto data = json::req_object(*root, "data");
            if (!data.has_value()) {
                return std::unexpected(data.error());
            }
            const auto market = json::req_str(*data, "m");
            if (!market.has_value()) {
                return std::unexpected(market.error());
            }
            out.market = *market;
            const auto bids = read_levels(*data, "b", size_key, storage.bids);
            if (!bids.has_value()) {
                return std::unexpected(bids.error());
            }
            const auto asks = read_levels(*data, "a", size_key, storage.asks);
            if (!asks.has_value()) {
                return std::unexpected(asks.error());
            }
            out.bids = {storage.bids.data(), *bids};
            out.asks = {storage.asks.data(), *asks};
        }
        const auto stamp = read_stamp(*root);
        if (!stamp.has_value()) {
            return std::unexpected(stamp.error());
        }
        out.seq = stamp->seq;
        out.ts_ms = stamp->ts_ms;
        return out;
    });
}

std::expected<WirePrints, WireError> parse_trades(std::string_view frame, PrintBuffer& storage) {
    return json::guarded([&]() -> std::expected<WirePrints, WireError> {
        auto root = json::session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        std::size_t count = 0;
        {
            auto data = json::req_array(*root, "data");
            if (!data.has_value()) {
                return std::unexpected(data.error());
            }
            for (auto entry : *data) {
                if (count == k_max_prints) {
                    return std::unexpected(WireError::TooManyEntries);
                }
                json::ondemand::object obj;
                if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                    return std::unexpected(json::map_error(ec));
                }
                WirePrint print{};
                const auto id = json::opt_u64(obj, "i");
                const auto market = json::req_str(obj, "m");
                const auto side = json::req_str(obj, "S");
                const auto ts = json::opt_u64(obj, "T");
                const auto price = json::req_str(obj, "p");
                const auto qty = json::req_str(obj, "q");
                if (!id.has_value() || !market.has_value() || !side.has_value() || !ts.has_value() ||
                    !price.has_value() || !qty.has_value()) {
                    return std::unexpected(WireError::MissingField);
                }
                if (*side != "BUY" && *side != "SELL") {
                    return std::unexpected(WireError::WrongType);
                }
                print.id = *id;
                print.market = *market;
                print.taker_bought = *side == "BUY";
                print.ts_ms = *ts;
                print.price = *price;
                print.qty = *qty;
                storage.prints[count] = print;
                ++count;
            }
        }
        const auto stamp = read_stamp(*root);
        if (!stamp.has_value()) {
            return std::unexpected(stamp.error());
        }
        return WirePrints{.seq = stamp->seq, .prints = {storage.prints.data(), count}};
    });
}

std::expected<WireMark, WireError> parse_mark(std::string_view frame) {
    return json::guarded([&]() -> std::expected<WireMark, WireError> {
        auto root = json::session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        WireMark out{};
        {
            auto data = json::req_object(*root, "data");
            if (!data.has_value()) {
                return std::unexpected(data.error());
            }
            const auto market = json::req_str(*data, "m");
            const auto price = json::req_str(*data, "p");
            if (!market.has_value() || !price.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            out.market = *market;
            out.price = *price;
        }
        const auto stamp = read_stamp(*root);
        if (!stamp.has_value()) {
            return std::unexpected(stamp.error());
        }
        out.seq = stamp->seq;
        out.ts_ms = stamp->ts_ms;
        return out;
    });
}

std::expected<WireFunding, WireError> parse_funding(std::string_view frame) {
    return json::guarded([&]() -> std::expected<WireFunding, WireError> {
        auto root = json::session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        WireFunding out{};
        {
            auto data = json::req_object(*root, "data");
            if (!data.has_value()) {
                return std::unexpected(data.error());
            }
            const auto market = json::req_str(*data, "m");
            const auto rate = json::req_str(*data, "f");
            if (!market.has_value() || !rate.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            out.market = *market;
            out.rate = *rate;
        }
        const auto stamp = read_stamp(*root);
        if (!stamp.has_value()) {
            return std::unexpected(stamp.error());
        }
        out.seq = stamp->seq;
        out.ts_ms = stamp->ts_ms;
        return out;
    });
}

} // namespace raikiri::venues::extended
