#include <raikiri/venues/hyperliquid/envelope.hpp>
#include <raikiri/venues/hyperliquid/market_data.hpp>
#include <raikiri/wire/json.hpp>

#include <utility>

namespace raikiri::venues::hyperliquid {

namespace {

using wire::guarded;
using wire::session;

// the same object arrives in activeAssetCtx `data.ctx` and as a metaAndAssetCtxs row
std::expected<void, WireError> read_ctx_fields(wire::ondemand::object& ctx, WireAssetCtx& out) {
    for (const auto& [key, field] : {std::pair{"funding", &out.funding},
                                     std::pair{"markPx", &out.mark_price},
                                     std::pair{"midPx", &out.mid_price},
                                     std::pair{"oraclePx", &out.oracle_price},
                                     std::pair{"prevDayPx", &out.prev_day_price},
                                     std::pair{"openInterest", &out.open_interest},
                                     std::pair{"dayBaseVlm", &out.day_base_volume},
                                     std::pair{"dayNtlVlm", &out.day_notional_volume}}) {
        const auto value = wire::opt_str(ctx, key);
        if (!value.has_value()) {
            return std::unexpected(value.error());
        }
        *field = *value;
    }
    return {};
}

} // namespace

std::expected<std::span<const WireTrade>, WireError> parse_trades(std::string_view frame,
                                                                  TradeBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireTrade>, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto array = wire::req_array(*root, "data");
        if (!array.has_value()) {
            return std::unexpected(array.error());
        }
        std::size_t count = 0;
        for (auto entry : *array) {
            if (count == k_max_trades) {
                return std::unexpected(WireError::TooManyEntries);
            }
            wire::ondemand::object obj;
            if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
            }
            WireTrade out{};
            const auto coin = wire::req_str(obj, "coin");
            const auto price = wire::req_str(obj, "px");
            const auto qty = wire::req_str(obj, "sz");
            const auto side = wire::req_str(obj, "side");
            const auto trade_id = wire::opt_raw(obj, "tid");
            const auto ts = opt_number_ms(obj, "time");
            if (!coin.has_value() || !price.has_value() || !qty.has_value() || !side.has_value() ||
                !trade_id.has_value() || !ts.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            out.coin = *coin;
            out.price = *price;
            out.qty = *qty;
            out.trade_id = *trade_id;
            out.aggressor_bought = *side == "B";
            out.ts_ms = *ts;
            storage.trades[count] = out;
            ++count;
        }
        return std::span<const WireTrade>{storage.trades.data(), count};
    });
}

std::expected<WireAssetCtx, WireError> parse_asset_ctx(std::string_view frame) {
    return guarded([&]() -> std::expected<WireAssetCtx, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        return with_data_object(
            *root, [](wire::ondemand::object& obj) -> std::expected<WireAssetCtx, WireError> {
                WireAssetCtx out{};
                const auto coin = wire::req_str(obj, "coin");
                if (!coin.has_value()) {
                    return std::unexpected(coin.error());
                }
                out.coin = *coin;
                auto ctx = wire::req_object(obj, "ctx");
                if (!ctx.has_value()) {
                    return std::unexpected(ctx.error());
                }
                if (const auto ok = read_ctx_fields(*ctx, out); !ok.has_value()) {
                    return std::unexpected(ok.error());
                }
                return out;
            });
    });
}

std::expected<std::span<const WireAssetCtx>, WireError> parse_asset_ctxs(std::string_view body,
                                                                         AssetCtxBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireAssetCtx>, WireError> {
        auto root = session().open_array(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        std::size_t element = 0;
        std::size_t count = 0;
        for (auto entry : *root) {
            if (element++ == 0) {
                continue;
            }
            wire::ondemand::array contexts;
            if (const auto ec = entry.get_array().get(contexts); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
            }
            for (auto row : contexts) {
                if (count == k_max_asset_ctxs) {
                    return std::unexpected(WireError::TooManyEntries);
                }
                wire::ondemand::object obj;
                if (const auto ec = row.get_object().get(obj); ec != simdjson::SUCCESS) {
                    return std::unexpected(wire::map_error(ec));
                }
                WireAssetCtx out{};
                if (const auto ok = read_ctx_fields(obj, out); !ok.has_value()) {
                    return std::unexpected(ok.error());
                }
                storage.ctxs[count] = out;
                ++count;
            }
            break;
        }
        if (element < 2) {
            return std::unexpected(WireError::MissingField);
        }
        return std::span<const WireAssetCtx>{storage.ctxs.data(), count};
    });
}

} // namespace raikiri::venues::hyperliquid
