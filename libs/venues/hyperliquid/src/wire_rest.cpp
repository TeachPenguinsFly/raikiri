#include "wire_detail.hpp"

namespace raikiri::venues::hyperliquid {

namespace {

using detail::guarded;
using detail::session;

std::expected<std::size_t, WireError> read_universe(detail::ondemand::object& meta,
                                                    AssetBuffer& storage) {
    auto array = detail::req_array(meta, "universe");
    if (!array.has_value()) {
        return std::unexpected(array.error());
    }
    std::size_t count = 0;
    for (auto entry : *array) {
        if (count == k_max_assets) {
            return std::unexpected(WireError::TooManyEntries);
        }
        detail::ondemand::object obj;
        if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
            return std::unexpected(detail::map_error(ec));
        }
        WireAsset out{};
        const auto name = detail::req_str(obj, "name");
        const auto sz_decimals = detail::opt_i64(obj, "szDecimals");
        const auto max_leverage = detail::opt_i64(obj, "maxLeverage");
        const auto delisted = detail::opt_bool(obj, "isDelisted");
        if (!name.has_value() || !sz_decimals.has_value() || !max_leverage.has_value() ||
            !delisted.has_value()) {
            return std::unexpected(WireError::MissingField);
        }
        out.name = *name;
        out.sz_decimals = *sz_decimals;
        out.max_leverage = *max_leverage;
        out.delisted = *delisted;
        storage.assets[count] = out;
        ++count;
    }
    return count;
}

} // namespace

std::expected<std::span<const WireAsset>, WireError> parse_meta(std::string_view body,
                                                                AssetBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireAsset>, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto count = read_universe(*root, storage);
        if (!count.has_value()) {
            return std::unexpected(count.error());
        }
        return std::span<const WireAsset>{storage.assets.data(), *count};
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
            detail::ondemand::array contexts;
            if (const auto ec = entry.get_array().get(contexts); ec != simdjson::SUCCESS) {
                return std::unexpected(detail::map_error(ec));
            }
            for (auto row : contexts) {
                if (count == k_max_assets) {
                    return std::unexpected(WireError::TooManyEntries);
                }
                detail::ondemand::object obj;
                if (const auto ec = row.get_object().get(obj); ec != simdjson::SUCCESS) {
                    return std::unexpected(detail::map_error(ec));
                }
                WireAssetCtx out{};
                if (const auto ok = detail::read_ctx_fields(obj, out); !ok.has_value()) {
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
