#include <raikiri/log/log.hpp>
#include <raikiri/net/rest_await.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/venues/lbank/endpoints.hpp>
#include <raikiri/venues/lbank/map.hpp>
#include <raikiri/venues/lbank/wire.hpp>

#include <algorithm>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "clients.hpp"

namespace raikiri::venues::lbank::internal {

namespace {

constexpr std::uint32_t k_catalogue_timeout_ms = 10'000;

// the book filter spells a tick as a plain decimal with at least one fraction digit
std::string tick_text(FixedE9 tick) {
    const auto whole = tick.raw / k_e9;
    auto frac = std::to_string(k_e9 + tick.raw % k_e9).substr(1);
    while (frac.size() > 1 && frac.back() == '0') {
        frac.pop_back();
    }
    return std::to_string(whole) + "." + frac;
}

class LbankInstruments final : public exec::InstrumentSource {
public:
    explicit LbankInstruments(std::shared_ptr<VenueState> state)
        : state_{std::move(state)},
          rest_{state_->io,
                net::RestConfig{
                    .host = std::string{state_->market == MarketType::Spot ? ep::k_spot_host : ep::k_perp_rest_host},
                    .port = "443",
                    .pool_size = 1,
                    .timeout_ms = k_catalogue_timeout_ms}} {}

    std::expected<std::vector<Instrument>, exec::VenueError> fetch_all(MarketType market) override {
        if (market != state_->market) {
            return std::unexpected{exec::not_implemented("lbank: this gateway serves one market")};
        }
        rest_.start();
        return market == MarketType::Spot ? load_spot() : load_perp();
    }

private:
    std::expected<std::string, exec::VenueError> get(std::string_view target) {
        const net::RestRequest request{.method = net::HttpMethod::Get, .target = target, .body = {}};
        const auto response = net::await_response(
            state_->io, *state_->clock, rest_, request, 2ULL * k_catalogue_timeout_ms + 1'000);
        if (!response.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Connectivity, 0, "catalogue: no response before deadline")};
        }
        if (response->transport_failed()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Connectivity, 0, response->error)};
        }
        if (response->status == 429) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::RateLimited, response->status, "catalogue: rate limited")};
        }
        if (response->status != 200) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, response->status, "catalogue: unexpected status")};
        }
        return std::string{response->body};
    }

    static exec::VenueError broken(WireError error) {
        return exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(error));
    }

    std::optional<InstrumentId> add(const Instrument& instrument) {
        const auto id = state_->instruments.add(instrument);
        if (!id.has_value()) {
            return std::nullopt;
        }
        if (state_->markets.size() <= id->symbol().raw) {
            state_->markets.resize(id->symbol().raw + 1);
        }
        return *id;
    }

    std::expected<std::vector<Instrument>, exec::VenueError> load_perp() {
        const auto markets_body = get(ep::k_perp_market_data);
        if (!markets_body.has_value()) {
            return std::unexpected{markets_body.error()};
        }
        const auto market_rows = std::make_unique<PerpMarketBuffer>();
        const auto markets = parse_perp_markets(*markets_body, *market_rows);
        if (!markets.has_value()) {
            return std::unexpected{broken(markets.error())};
        }
        std::unordered_map<std::string, std::uint32_t> intervals;
        for (const auto& row : *markets) {
            intervals.emplace(row.symbol, row.interval_s);
        }

        const auto body = get(ep::k_perp_instruments);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        const auto storage = std::make_unique<PerpInstrumentBuffer>();
        const auto rows = parse_perp_instruments(*body, *storage);
        if (!rows.has_value()) {
            return std::unexpected{broken(rows.error())};
        }
        std::vector<Instrument> loaded;
        loaded.reserve(rows->size());
        state_->instruments.reserve(rows->size());
        std::size_t suspended = 0;
        for (const auto& row : *rows) {
            suspended += row.suspended ? 1U : 0U;
            const auto interval = intervals.find(std::string{row.symbol});
            const auto instrument = map_perp_instrument(row, interval == intervals.end() ? 0U : interval->second);
            if (!instrument.has_value()) {
                continue;
            }
            const auto id = add(*instrument);
            if (!id.has_value()) {
                continue;
            }
            const auto& base = base_of(*instrument);
            auto& market = state_->markets[id->symbol().raw];
            market.interval_s = interval == intervals.end() ? 0U : interval->second;
            market.tick_text = tick_text(base.tick_size);
            loaded.push_back(*state_->instruments.find(*id));
        }
        if (::raikiri::log::logger() != nullptr) {
            RLOG_INFO("lbank: {} contracts listed, {} suspended, {} loaded; the contract api "
                      "publishes no order entry, so 0 are tradable through it",
                      rows->size(),
                      suspended,
                      loaded.size());
        }
        return loaded;
    }

    std::expected<std::vector<Instrument>, exec::VenueError> load_spot() {
        const auto tickers_body = get(ep::k_spot_tickers);
        if (!tickers_body.has_value()) {
            return std::unexpected{tickers_body.error()};
        }
        const auto symbols = std::make_unique<SymbolBuffer>();
        const auto quoted = parse_spot_ticker_symbols(*tickers_body, *symbols);
        if (!quoted.has_value()) {
            return std::unexpected{broken(quoted.error())};
        }
        std::unordered_set<std::string> live;
        for (const auto symbol : *quoted) {
            live.emplace(symbol);
        }

        const auto body = get(ep::k_spot_accuracy);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        const auto storage = std::make_unique<SpotPairBuffer>();
        const auto rows = parse_spot_pairs(*body, *storage);
        if (!rows.has_value()) {
            return std::unexpected{broken(rows.error())};
        }
        std::vector<Instrument> loaded;
        loaded.reserve(rows->size());
        state_->instruments.reserve(rows->size());
        std::size_t unquoted = 0;
        for (const auto& row : *rows) {
            if (!live.contains(std::string{row.symbol})) {
                ++unquoted;
                continue;
            }
            const auto instrument = map_spot_instrument(row);
            if (!instrument.has_value()) {
                continue;
            }
            if (const auto id = add(*instrument); id.has_value()) {
                loaded.push_back(*state_->instruments.find(*id));
            }
        }
        if (::raikiri::log::logger() != nullptr) {
            RLOG_INFO("lbank: {} spot pairs listed, {} without a ticker left out, {} loaded",
                      rows->size(),
                      unquoted,
                      loaded.size());
        }
        return loaded;
    }

    std::shared_ptr<VenueState> state_;
    net::RestClient rest_;
};

} // namespace

std::unique_ptr<exec::InstrumentSource> make_instrument_source(std::shared_ptr<VenueState> state) {
    return std::make_unique<LbankInstruments>(std::move(state));
}

} // namespace raikiri::venues::lbank::internal
