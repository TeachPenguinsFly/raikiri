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
        // OPTIONAL, unlike the quote columns above: a product that has not
        // traded in the window is still a product with a book, and refusing
        // the frame over an absent volume would drop its top of book too.
        out.volume_24h = wire::opt_str(*root, "volume_24h").value_or(std::string_view{});
        return out;
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
        if (!product.has_value() || !price.has_value() || !size.has_value() || !time.has_value() ||
            !side.has_value()) {
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
