#include <raikiri/venues/kraken/catalogue.hpp>
#include <raikiri/venues/kraken/envelope.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::kraken {

namespace {

using wire::guarded;
using wire::session;

} // namespace

std::expected<std::span<const WireInstrument>, WireError> parse_instruments(std::string_view body,
                                                                            InstrumentBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireInstrument>, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto array = wire::req_array(*root, "instruments");
        if (!array.has_value()) {
            return std::unexpected(array.error());
        }
        std::size_t count = 0;
        for (auto entry : *array) {
            if (count == k_max_instruments) {
                return std::unexpected(WireError::TooManyEntries);
            }
            wire::ondemand::object obj;
            if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
            }
            WireInstrument out{};
            const auto symbol = wire::req_str(obj, "symbol");
            const auto type = wire::req_str(obj, "type");
            if (!symbol.has_value() || !type.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            out.symbol = *symbol;
            out.type = *type;
            const auto tick = opt_num(obj, "tickSize");
            const auto contract = opt_num(obj, "contractSize");
            const auto underlying = wire::opt_str(obj, "underlying");
            const auto last_trading = wire::opt_str(obj, "lastTradingTime");
            const auto tradeable = wire::opt_bool(obj, "tradeable");
            const auto base = wire::opt_str(obj, "base");
            const auto quote = wire::opt_str(obj, "quote");
            if (!tick.has_value() || !contract.has_value() || !underlying.has_value() || !last_trading.has_value() ||
                !tradeable.has_value() || !base.has_value() || !quote.has_value()) {
                return std::unexpected(WireError::WrongType);
            }
            out.base = *base;
            out.quote = *quote;
            out.tick_size = *tick;
            out.contract_size = *contract;
            out.underlying = *underlying;
            out.last_trading_time = *last_trading;
            out.tradeable = *tradeable;
            const auto precision = opt_i64(obj, "contractValueTradePrecision", out.has_precision);
            if (!precision.has_value()) {
                return std::unexpected(precision.error());
            }
            out.contract_value_trade_precision = *precision;
            storage.instruments[count] = out;
            ++count;
        }
        return std::span<const WireInstrument>{storage.instruments.data(), count};
    });
}

} // namespace raikiri::venues::kraken
