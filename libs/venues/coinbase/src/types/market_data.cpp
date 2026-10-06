#include <raikiri/venues/coinbase/market_data.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::coinbase {

namespace {

using wire::guarded;
using wire::session;

} // namespace

std::expected<WireTicker, WireError> parse_ticker(std::string_view frame) {
    return guarded([&]() -> std::expected<WireTicker, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        WireTicker out{};
        const auto product = wire::req_str(*root, "product_id");
        const auto bid = wire::req_str(*root, "best_bid");
        const auto bid_size = wire::req_str(*root, "best_bid_size");
        const auto ask = wire::req_str(*root, "best_ask");
        const auto ask_size = wire::req_str(*root, "best_ask_size");
        const auto time = wire::req_str(*root, "time");
        if (!product.has_value() || !bid.has_value() || !bid_size.has_value() || !ask.has_value() ||
            !ask_size.has_value() || !time.has_value()) {
            return std::unexpected(WireError::MissingField);
        }
        out.product_id = *product;
        out.best_bid = *bid;
        out.best_bid_size = *bid_size;
        out.best_ask = *ask;
        out.best_ask_size = *ask_size;
        out.time = *time;
        const auto sequence = wire::opt_i64(*root, "sequence");
        const auto trade_id = wire::opt_i64(*root, "trade_id");
        if (!sequence.has_value() || !trade_id.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        out.sequence = *sequence;
        out.trade_id = *trade_id;
        // optional: a product with no trades in the window still has a book
        out.volume_24h = wire::opt_str(*root, "volume_24h").value_or(std::string_view{});
        return out;
    });
}

std::expected<std::span<const WireProductVolume>, WireError> parse_product_volumes(std::string_view body,
                                                                                   ProductVolumeBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireProductVolume>, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto array = wire::req_array(*root, "products");
        if (!array.has_value()) {
            return std::unexpected(array.error());
        }
        std::size_t count = 0;
        for (auto entry : *array) {
            if (count == storage.rows.size()) {
                return std::unexpected(WireError::TooManyEntries);
            }
            wire::ondemand::object obj;
            if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
            }
            WireProductVolume out{};
            const auto id = wire::req_str(obj, "product_id");
            if (!id.has_value()) {
                return std::unexpected(id.error());
            }
            out.product_id = *id;
            const auto volume = wire::opt_str(obj, "volume_24h");
            const auto quote = wire::opt_str(obj, "approximate_quote_24h_volume");
            if (!volume.has_value() || !quote.has_value()) {
                return std::unexpected(WireError::WrongType);
            }
            out.volume_24h = *volume;
            out.quote_volume_24h = *quote;
            storage.rows[count] = out;
            ++count;
        }
        return std::span<const WireProductVolume>{storage.rows.data(), count};
    });
}

std::expected<WireMatch, WireError> parse_match(std::string_view frame) {
    return guarded([&]() -> std::expected<WireMatch, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        WireMatch out{};
        const auto product = wire::req_str(*root, "product_id");
        const auto price = wire::req_str(*root, "price");
        const auto size = wire::req_str(*root, "size");
        const auto time = wire::req_str(*root, "time");
        const auto side = wire::req_str(*root, "side");
        if (!product.has_value() || !price.has_value() || !size.has_value() || !time.has_value() || !side.has_value()) {
            return std::unexpected(WireError::MissingField);
        }
        out.product_id = *product;
        out.price = *price;
        out.size = *size;
        out.time = *time;
        out.maker_sold = *side == "sell";
        const auto sequence = wire::opt_i64(*root, "sequence");
        const auto trade_id = wire::opt_i64(*root, "trade_id");
        if (!sequence.has_value() || !trade_id.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        out.sequence = *sequence;
        out.trade_id = *trade_id;
        return out;
    });
}

} // namespace raikiri::venues::coinbase
