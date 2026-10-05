// loads venue catalogues the way a gateway does and prints each listing with its asset key, one tab-separated
// row per instrument, for scripts/symbol_audit.py to price and compare

#include <raikiri/admin/format.hpp>
#include <raikiri/domain/asset_class.hpp>
#include <raikiri/exec/instrument_source.hpp>
#include <raikiri/exec/venue.hpp>
#include <raikiri/log/log.hpp>
#include <raikiri/venues/apex/apex_venue.hpp>
#include <raikiri/venues/aster/aster_venue.hpp>
#include <raikiri/venues/backpack/backpack_venue.hpp>
#include <raikiri/venues/binance/binance_venue.hpp>
#include <raikiri/venues/bingx/bingx_venue.hpp>
#include <raikiri/venues/bitfinex/bitfinex_venue.hpp>
#include <raikiri/venues/bitget/bitget_venue.hpp>
#include <raikiri/venues/bitunix/bitunix_venue.hpp>
#include <raikiri/venues/blofin/blofin_venue.hpp>
#include <raikiri/venues/bybit/bybit_venue.hpp>
#include <raikiri/venues/coinbase/coinbase_venue.hpp>
#include <raikiri/venues/cryptocom/cryptocom_venue.hpp>
#include <raikiri/venues/deribit/deribit_venue.hpp>
#include <raikiri/venues/derive/derive_venue.hpp>
#include <raikiri/venues/dydx/dydx_venue.hpp>
#include <raikiri/venues/edgex/edgex_venue.hpp>
#include <raikiri/venues/extended/extended_venue.hpp>
#include <raikiri/venues/gate/gate_venue.hpp>
#include <raikiri/venues/grvt/grvt_venue.hpp>
#include <raikiri/venues/htx/htx_venue.hpp>
#include <raikiri/venues/hyperliquid/hyperliquid_venue.hpp>
#include <raikiri/venues/kraken/kraken_venue.hpp>
#include <raikiri/venues/kucoin/kucoin_venue.hpp>
#include <raikiri/venues/lbank/lbank_venue.hpp>
#include <raikiri/venues/lighter/lighter_venue.hpp>
#include <raikiri/venues/mexc/mexc_venue.hpp>
#include <raikiri/venues/okx/okx_venue.hpp>
#include <raikiri/venues/pacifica/pacifica_venue.hpp>
#include <raikiri/venues/phemex/phemex_venue.hpp>
#include <raikiri/venues/toobit/toobit_venue.hpp>
#include <raikiri/venues/weex/weex_venue.hpp>
#include <raikiri/venues/xt/xt_venue.hpp>

#include <fmt/format.h>

#include <chrono>
#include <cstdio>
#include <string_view>

namespace {

using namespace raikiri;

constexpr std::string_view k_usage = R"(usage: symbolaudit [--feed=<adapter feed>] <account>...
  prints account, venue, market, exchange symbol, local symbol, asset class, underlying, asset key,
  market key, dex, contract value and contract kind for every listing, tab-separated
  accounts are named as gatewayd names them: binance, bybit-spot, hyperliquid; --feed is gatewayd's,
  so --feed=xyz,para also loads those hyperliquid deployers
)";

class NullSink final : public exec::GatewaySink {
public:
    void on_bbo(const Bbo&) noexcept override {}

    void on_trade(const TradeTick&) noexcept override {}

    void on_funding(const FundingUpdate&) noexcept override {}

    void on_order_event(const OrderEvent&) noexcept override {}

    void on_connectivity(const exec::ConnEvent&) noexcept override {}
};

void register_all() {
    venues::bybit::register_adapter();
    venues::binance::register_adapter();
    venues::bitget::register_adapter();
    venues::htx::register_adapter();
    venues::bitfinex::register_adapter();
    venues::deribit::register_adapter();
    venues::hyperliquid::register_adapter();
    venues::coinbase::register_adapter();
    venues::kraken::register_adapter();
    venues::gate::register_adapter();
    venues::kucoin::register_adapter();
    venues::bingx::register_adapter();
    venues::xt::register_adapter();
    venues::mexc::register_adapter();
    venues::okx::register_adapter();
    venues::weex::register_adapter();
    venues::aster::register_adapter();
    venues::grvt::register_adapter();
    venues::lighter::register_adapter();
    venues::backpack::register_adapter();
    venues::cryptocom::register_adapter();
    venues::blofin::register_adapter();
    venues::apex::register_adapter();
    venues::edgex::register_adapter();
    venues::dydx::register_adapter();
    venues::phemex::register_adapter();
    venues::extended::register_adapter();
    venues::pacifica::register_adapter();
    venues::lbank::register_adapter();
    venues::bitunix::register_adapter();
    venues::toobit::register_adapter();
    venues::derive::register_adapter();
}

bool dump(std::string_view name, const std::string& feed, const Clock& clock) {
    const auto account = account_from_string(name);
    if (account.venue == VenueId::Invalid) {
        fmt::print(stderr, "symbolaudit: {} is not an account name\n", name);
        return false;
    }
    NullSink sink;
    auto adapter = exec::make_venue(
        {.venue = account.venue, .market = account.market, .market_data_feed = feed}, sink, clock);
    if (!adapter.has_value()) {
        fmt::print(stderr, "symbolaudit: {}: {}\n", name, exec::to_string(adapter.error()));
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{90};
    auto loaded = exec::load_catalogue(
        *adapter->instruments,
        account.market,
        [&] { return std::chrono::steady_clock::now() >= deadline; },
        [&](const exec::VenueError& error, std::uint32_t, std::chrono::milliseconds) {
            fmt::print(stderr, "symbolaudit: {}: retrying after {}\n", name, error.message.view());
        });
    if (!loaded.has_value()) {
        fmt::print(stderr, "symbolaudit: {}: {}\n", name, loaded.error().message.view());
        return false;
    }
    for (const auto& listed : *loaded) {
        const auto& base = base_of(listed);
        fmt::print("{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\n",
                   name,
                   to_string(account.venue),
                   to_string(account.market),
                   base.exchange_symbol.view(),
                   base.local_symbol.view(),
                   to_string(base.asset_class),
                   base.underlying.view(),
                   asset_key(base),
                   market_key(base),
                   dex_of(base),
                   admin::decimal(base.contract_value),
                   admin::contract_kind_name(base.kind));
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fputs(k_usage.data(), stderr);
        return 2;
    }
    for (int i = 1; i < argc; ++i) {
        if (const std::string_view arg{argv[i]}; arg == "--help" || arg == "-h") {
            std::fputs(k_usage.data(), stdout);
            return 0;
        }
    }
    const raikiri::log::Scope logging{"symbolaudit"};
    register_all();
    const LiveClock clock;
    std::string feed;
    int failed = 0;
    for (int i = 1; i < argc; ++i) {
        if (const std::string_view arg{argv[i]}; arg.starts_with("--feed=")) {
            feed = arg.substr(7);
            continue;
        }
        failed += dump(argv[i], feed, clock) ? 0 : 1;
    }
    return failed == 0 ? 0 : 1;
}
