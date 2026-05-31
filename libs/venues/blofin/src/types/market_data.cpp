#include <raikiri/venues/blofin/envelope.hpp>
#include <raikiri/venues/blofin/market_data.hpp>
#include <raikiri/wire/json.hpp>

#include <utility>

namespace raikiri::venues::blofin {

std::expected<std::span<const WirePrint>, WireError> parse_trades(std::string_view frame,
                                                                  PrintBuffer& storage) {
    return read_data<WirePrint>(
        frame,
        storage.prints,
        [](wire::ondemand::object& obj, WirePrint& out) -> std::expected<void, WireError> {
            const auto inst_id = wire::req_str(obj, "instId");
            const auto price = wire::req_str(obj, "price");
            const auto size = wire::req_str(obj, "size");
            const auto side = wire::req_str(obj, "side");
            if (!inst_id.has_value() || !price.has_value() || !size.has_value() ||
                !side.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            if (*side != "buy" && *side != "sell") {
                return std::unexpected(WireError::WrongType);
            }
            const auto ts = wire::opt_ms(obj, "ts");
            if (!ts.has_value()) {
                return std::unexpected(ts.error());
            }
            out.inst_id = *inst_id;
            out.price = *price;
            out.size = *size;
            out.taker_bought = *side == "buy";
            out.ts_ms = *ts;
            return {};
        });
}

std::expected<std::span<const WireTicker>, WireError> parse_tickers(std::string_view frame,
                                                                    TickerBuffer& storage) {
    return read_data<WireTicker>(
        frame,
        storage.rows,
        [](wire::ondemand::object& obj, WireTicker& out) -> std::expected<void, WireError> {
            const auto inst_id = wire::req_str(obj, "instId");
            if (!inst_id.has_value()) {
                return std::unexpected(inst_id.error());
            }
            const auto vol = wire::opt_str(obj, "vol24h");
            const auto vol_ccy = wire::opt_str(obj, "volCurrency24h");
            const auto last = wire::opt_str(obj, "last");
            const auto ts = wire::opt_ms(obj, "ts");
            if (!vol.has_value() || !vol_ccy.has_value() || !last.has_value() || !ts.has_value()) {
                return std::unexpected(WireError::WrongType);
            }
            out.inst_id = *inst_id;
            out.vol_contracts = *vol;
            out.vol_currency = *vol_ccy;
            out.last = *last;
            out.ts_ms = *ts;
            return {};
        });
}

std::expected<std::span<const WireFundingRow>, WireError> parse_funding(std::string_view body,
                                                                        FundingBuffer& storage) {
    return read_data<WireFundingRow>(
        body,
        storage.rows,
        [](wire::ondemand::object& obj, WireFundingRow& out) -> std::expected<void, WireError> {
            const auto inst_id = wire::req_str(obj, "instId");
            if (!inst_id.has_value()) {
                return std::unexpected(inst_id.error());
            }
            out.inst_id = *inst_id;
            for (const auto& [key, field] : {std::pair{"fundingRate", &out.funding_rate},
                                             std::pair{"fundingInterval", &out.interval},
                                             std::pair{"fundingIntervalUnit", &out.interval_unit},
                                             std::pair{"fundingRateCap", &out.cap},
                                             std::pair{"fundingRateFloor", &out.floor}}) {
                const auto value = wire::opt_str(obj, key);
                if (!value.has_value()) {
                    return std::unexpected(value.error());
                }
                *field = *value;
            }
            const auto funding = wire::opt_ms(obj, "fundingTime");
            if (!funding.has_value()) {
                return std::unexpected(funding.error());
            }
            out.funding_ms = *funding;
            return {};
        });
}

std::expected<std::span<const WireMark>, WireError> parse_mark_table(std::string_view body,
                                                                     MarkBuffer& storage) {
    return read_data<WireMark>(
        body,
        storage.rows,
        [](wire::ondemand::object& obj, WireMark& out) -> std::expected<void, WireError> {
            const auto inst_id = wire::req_str(obj, "instId");
            const auto mark = wire::opt_str(obj, "markPrice");
            const auto ts = wire::opt_ms(obj, "ts");
            if (!inst_id.has_value() || !mark.has_value() || !ts.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            out.inst_id = *inst_id;
            out.mark_price = *mark;
            out.ts_ms = *ts;
            return {};
        });
}

} // namespace raikiri::venues::blofin
