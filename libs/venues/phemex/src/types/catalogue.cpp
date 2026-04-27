#include <raikiri/venues/phemex/catalogue.hpp>
#include <raikiri/wire/json.hpp>

#include <initializer_list>

namespace raikiri::venues::phemex {

namespace detail = raikiri::wire;

namespace {

using detail::guarded;
using detail::session;

struct Text {
    std::string_view key;
    std::string_view* into;
};

std::expected<void, WireError> read_text(detail::ondemand::object& obj,
                                         std::initializer_list<Text> wanted) {
    for (const auto& want : wanted) {
        const auto value = detail::opt_str(obj, want.key);
        if (!value.has_value()) {
            return std::unexpected(value.error());
        }
        *want.into = *value;
    }
    return {};
}

template <typename Int> struct Number {
    std::string_view key;
    Int* into;
};

std::expected<void, WireError> read_u32(detail::ondemand::object& obj,
                                        std::initializer_list<Number<std::uint32_t>> wanted) {
    for (const auto& want : wanted) {
        const auto value = detail::opt_u64(obj, want.key);
        if (!value.has_value()) {
            return std::unexpected(value.error());
        }
        *want.into = static_cast<std::uint32_t>(*value);
    }
    return {};
}

std::expected<void, WireError> read_i64(detail::ondemand::object& obj,
                                        std::initializer_list<Number<std::int64_t>> wanted) {
    for (const auto& want : wanted) {
        const auto value = detail::opt_i64(obj, want.key);
        if (!value.has_value()) {
            return std::unexpected(value.error());
        }
        *want.into = *value;
    }
    return {};
}

std::expected<WireCurrency, WireError> read_currency(detail::ondemand::object& obj) {
    WireCurrency out{};
    if (const auto ok = read_text(obj, {{"currency", &out.currency}}); !ok.has_value()) {
        return std::unexpected(ok.error());
    }
    if (const auto ok = read_u32(obj, {{"valueScale", &out.value_scale}}); !ok.has_value()) {
        return std::unexpected(ok.error());
    }
    return out;
}

std::expected<WireProduct, WireError> read_product(detail::ondemand::object& obj) {
    WireProduct out{};
    if (const auto ok = read_text(obj,
                                  {{"symbol", &out.symbol},
                                   {"type", &out.type},
                                   {"status", &out.status},
                                   {"baseCurrency", &out.base},
                                   {"quoteCurrency", &out.quote},
                                   {"settleCurrency", &out.settle}});
        !ok.has_value()) {
        return std::unexpected(ok.error());
    }
    if (const auto ok = read_u32(obj,
                                 {{"priceScale", &out.price_scale},
                                  {"ratioScale", &out.ratio_scale},
                                  {"fundingInterval", &out.funding_interval_s}});
        !ok.has_value()) {
        return std::unexpected(ok.error());
    }
    if (const auto ok = read_i64(obj,
                                 {{"lotSize", &out.lot_size},
                                  {"quoteTickSizeEv", &out.quote_tick_ev},
                                  {"baseTickSizeEv", &out.base_tick_ev},
                                  {"minOrderValueEv", &out.min_order_value_ev},
                                  {"defaultTakerFeeEr", &out.taker_fee_er},
                                  {"defaultMakerFeeEr", &out.maker_fee_er}});
        !ok.has_value()) {
        return std::unexpected(ok.error());
    }
    out.has_fees =
        detail::has_field(obj, "defaultTakerFeeEr") && detail::has_field(obj, "defaultMakerFeeEr");
    const auto tick = detail::opt_raw(obj, "tickSize");
    const auto contract = detail::opt_raw(obj, "contractSize");
    if (!tick.has_value() || !contract.has_value()) {
        return std::unexpected(WireError::WrongType);
    }
    out.tick_size = *tick;
    out.contract_size = *contract;
    return out;
}

std::expected<WirePerp, WireError> read_perp(detail::ondemand::object& obj) {
    WirePerp out{};
    if (const auto ok = read_text(obj,
                                  {{"symbol", &out.symbol},
                                   {"status", &out.status},
                                   {"perpProductSubType", &out.sub_type},
                                   {"baseCurrency", &out.base},
                                   {"quoteCurrency", &out.quote},
                                   {"tickSize", &out.tick_size},
                                   {"qtyStepSize", &out.qty_step},
                                   {"minOrderValueRv", &out.min_order_value}});
        !ok.has_value()) {
        return std::unexpected(ok.error());
    }
    if (const auto ok = read_u32(obj, {{"fundingInterval", &out.funding_interval_s}});
        !ok.has_value()) {
        return std::unexpected(ok.error());
    }
    return out;
}

// A row that will not read is skipped and counted rather than failing the
// catalogue: most of the ~2000 rows are delisted leftovers.
template <typename Row, typename Read>
std::expected<std::size_t, WireError> read_rows(detail::ondemand::object& data,
                                                std::string_view key,
                                                std::span<Row> into,
                                                std::size_t& malformed,
                                                Read read) {
    auto rows = detail::req_array(data, key);
    if (!rows.has_value()) {
        return std::unexpected(rows.error());
    }
    std::size_t count = 0;
    for (auto entry : *rows) {
        if (count == into.size()) {
            return std::unexpected(WireError::TooManyEntries);
        }
        detail::ondemand::object obj;
        if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
            return std::unexpected(detail::map_error(ec));
        }
        const auto row = read(obj);
        if (!row.has_value()) {
            ++malformed;
            continue;
        }
        into[count] = *row;
        ++count;
    }
    return count;
}

} // namespace

std::expected<WireCatalogue, WireError> parse_products(std::string_view body,
                                                       CatalogueBuffer& storage) {
    return guarded([&]() -> std::expected<WireCatalogue, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto code = detail::opt_i64(*root, "code");
        if (!code.has_value() || *code != 0) {
            return std::unexpected(WireError::UnknownMessage);
        }
        auto data = detail::req_object(*root, "data");
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        WireCatalogue out{};
        const auto currencies = read_rows(
            *data, "currencies", std::span<WireCurrency>{storage.currencies}, out.malformed, read_currency);
        if (!currencies.has_value()) {
            return std::unexpected(currencies.error());
        }
        const auto products = read_rows(
            *data, "products", std::span<WireProduct>{storage.products}, out.malformed, read_product);
        if (!products.has_value()) {
            return std::unexpected(products.error());
        }
        const auto perps =
            read_rows(*data, "perpProductsV2", std::span<WirePerp>{storage.perps}, out.malformed, read_perp);
        if (!perps.has_value()) {
            return std::unexpected(perps.error());
        }
        out.currencies = {storage.currencies.data(), *currencies};
        out.products = {storage.products.data(), *products};
        out.perps = {storage.perps.data(), *perps};
        return out;
    });
}

} // namespace raikiri::venues::phemex
