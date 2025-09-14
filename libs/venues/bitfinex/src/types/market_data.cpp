#include <raikiri/venues/bitfinex/envelope.hpp>
#include <raikiri/venues/bitfinex/market_data.hpp>

namespace raikiri::venues::bitfinex {

// The shared JSON readers. `raikiri::wire` is the deliberate simdjson
// layer; a venue that reimplements opt_str or req_array is one that will
// disagree with its neighbours about what an absent field means.
namespace detail = raikiri::wire;

namespace {

using detail::guarded;

std::expected<WireTrade, WireError> read_trade(detail::ondemand::value value) {
    const auto row = read_row<k_trade_arity>(value);
    if (!row.has_value()) {
        return std::unexpected(row.error());
    }
    WireTrade out{};
    out.id = row->template at<k_trade_id>();
    out.ts_ms = detail::digits_to_u64(row->template at<k_trade_mts>());
    out.amount = row->template at<k_trade_amount>();
    out.price = row->template at<k_trade_price>();
    return out;
}

} // namespace

std::expected<WireTicker, WireError> parse_ticker(std::string_view frame) {
    return guarded([&]() -> std::expected<WireTicker, WireError> {
        return with_payload(frame,
                            [](detail::ondemand::value payload,
                               std::string_view tag) -> std::expected<WireTicker, WireError> {
                                if (!tag.empty()) {
                                    return std::unexpected(WireError::WrongType);
                                }
                                const auto row = read_row<k_ticker_arity>(payload);
                                if (!row.has_value()) {
                                    return std::unexpected(row.error());
                                }
                                WireTicker out{};
                                out.bid = row->at<k_ticker_bid>();
                                out.bid_size = row->at<k_ticker_bid_size>();
                                out.ask = row->at<k_ticker_ask>();
                                out.ask_size = row->at<k_ticker_ask_size>();
                                out.last_price = row->at<k_ticker_last_price>();
                                out.volume = row->at<k_ticker_volume>();
                                return out;
                            });
    });
}

std::expected<WireTrades, WireError> parse_trades(std::string_view frame, TradeBuffer& storage) {
    return guarded([&]() -> std::expected<WireTrades, WireError> {
        return with_payload(
            frame,
            [&storage](detail::ondemand::value payload,
                       std::string_view tag) -> std::expected<WireTrades, WireError> {
                if (tag == "te" || tag == "tu") {
                    const auto trade = read_trade(payload);
                    if (!trade.has_value()) {
                        return std::unexpected(trade.error());
                    }
                    storage.trades[0] = *trade;
                    return WireTrades{.kind = tag == "te" ? TradeFrameKind::Executed
                                                          : TradeFrameKind::Updated,
                                      .trades = {storage.trades.data(), 1}};
                }
                if (!tag.empty()) {
                    return std::unexpected(WireError::WrongType);
                }
                detail::ondemand::array rows;
                if (const auto ec = payload.get_array().get(rows); ec != simdjson::SUCCESS) {
                    return std::unexpected(detail::map_error(ec));
                }
                std::size_t count = 0;
                for (auto entry : rows) {
                    if (count == k_max_trades) {
                        return std::unexpected(WireError::TooManyEntries);
                    }
                    detail::ondemand::value value;
                    if (const auto ec = entry.get(value); ec != simdjson::SUCCESS) {
                        return std::unexpected(detail::map_error(ec));
                    }
                    const auto trade = read_trade(value);
                    if (!trade.has_value()) {
                        return std::unexpected(trade.error());
                    }
                    storage.trades[count] = *trade;
                    ++count;
                }
                return WireTrades{.kind = TradeFrameKind::Snapshot,
                                  .trades = {storage.trades.data(), count}};
            });
    });
}

std::expected<WireDerivStatus, WireError> parse_deriv_status(std::string_view frame) {
    return guarded([&]() -> std::expected<WireDerivStatus, WireError> {
        return with_payload(frame,
                            [](detail::ondemand::value payload,
                               std::string_view tag) -> std::expected<WireDerivStatus, WireError> {
                                if (!tag.empty()) {
                                    return std::unexpected(WireError::WrongType);
                                }
                                const auto row = read_row<k_deriv_arity>(payload);
                                if (!row.has_value()) {
                                    return std::unexpected(row.error());
                                }
                                WireDerivStatus out{};
                                out.ts_ms = detail::digits_to_u64(row->at<k_deriv_mts>());
                                out.deriv_price = row->at<k_deriv_price>();
                                out.spot_price = row->at<k_deriv_spot_price>();
                                out.next_funding_ms =
                                    detail::digits_to_u64(row->at<k_deriv_next_funding_mts>());
                                out.next_funding_accrued = row->at<k_deriv_next_funding_accrued>();
                                out.current_funding = row->at<k_deriv_current_funding>();
                                out.mark_price = row->at<k_deriv_mark_price>();
                                out.open_interest = row->at<k_deriv_open_interest>();
                                return out;
                            });
    });
}

} // namespace raikiri::venues::bitfinex
