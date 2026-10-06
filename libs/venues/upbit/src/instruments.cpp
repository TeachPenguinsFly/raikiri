#include <raikiri/log/log.hpp>
#include <raikiri/net/rest_await.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/venues/upbit/endpoints.hpp>
#include <raikiri/venues/upbit/map.hpp>
#include <raikiri/venues/upbit/wire.hpp>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "clients.hpp"

namespace raikiri::venues::upbit::internal {

namespace {

constexpr std::uint32_t k_catalogue_timeout_ms = 15'000;

class UpbitInstruments final : public exec::InstrumentSource {
public:
    explicit UpbitInstruments(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, rest_{state_->io,
                                          net::RestConfig{.host = std::string{state_->flavour.rest_host},
                                                          .port = "443",
                                                          .pool_size = 1,
                                                          .timeout_ms = k_catalogue_timeout_ms}} {
        rest_.set_admit(exec::market_data_gate(state_->budget, *state_->clock));
    }

    std::expected<std::vector<Instrument>, exec::VenueError> fetch_all(MarketType market) override {
        if (market != MarketType::Spot) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, "spot only")};
        }
        rest_.start();
        const auto listing = get(ep::k_markets);
        if (!listing.has_value()) {
            return std::unexpected{listing.error()};
        }
        static thread_local const auto rows = std::make_unique<MarketBuffer>();
        const auto markets = parse_markets(*listing, *rows);
        if (!markets.has_value()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(markets.error()))};
        }
        std::vector<std::string> codes;
        codes.reserve(markets->size());
        for (const auto& row : *markets) {
            codes.emplace_back(row.market);
        }
        const auto prices = last_prices(codes);
        if (!prices.has_value()) {
            return std::unexpected{prices.error()};
        }

        const auto now = state_->clock->wall();
        std::vector<Instrument> loaded;
        loaded.reserve(codes.size());
        state_->instruments.reserve(codes.size());
        for (const auto& code : codes) {
            const auto price = prices->find(code);
            const auto instrument = map_instrument(WireMarket{.market = code},
                                                   price == prices->end() ? std::string_view{} : price->second,
                                                   state_->flavour.venue,
                                                   now);
            if (!instrument.has_value()) {
                continue;
            }
            const auto id = state_->instruments.add(*instrument);
            if (!id.has_value()) {
                continue;
            }
            if (state_->markets.size() <= id->symbol().raw) {
                state_->markets.resize(id->symbol().raw + 1);
            }
            loaded.push_back(*state_->instruments.find(*id));
        }
        if (::raikiri::log::logger() != nullptr) {
            RLOG_INFO("{}: {} markets listed, {} priced at load, {} loaded",
                      state_->flavour.name,
                      codes.size(),
                      prices->size(),
                      loaded.size());
        }
        return loaded;
    }

private:
    [[nodiscard]] std::expected<std::string, exec::VenueError> get(std::string_view target) {
        const net::RestRequest request{.method = net::HttpMethod::Get, .target = target};
        auto response = net::await_response(
            state_->io, *state_->clock, rest_, request, 2ULL * k_catalogue_timeout_ms + 1'000);
        if (!response.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Connectivity, 0, "catalogue: no response before deadline")};
        }
        if (response->transport_failed()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Connectivity, 0, response->error)};
        }
        if (response->status == 429) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::RateLimited, 429, "catalogue: throttled")};
        }
        if (response->status != 200) {
            const auto refusal = parse_refusal(response->body);
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal,
                                       response->status,
                                       refusal.has_value() ? refusal->message : "catalogue: refused")};
        }
        return std::move(response->body);
    }

    // the last price sizes the lot; a market the ticker leaves out keeps the finest lot
    [[nodiscard]] std::expected<std::unordered_map<std::string, std::string>, exec::VenueError>
    last_prices(const std::vector<std::string>& codes) {
        std::vector<std::string> targets;
        if (!state_->flavour.ticker_all.empty()) {
            targets.emplace_back(state_->flavour.ticker_all);
        } else {
            for (std::size_t at = 0; at < codes.size(); at += state_->flavour.ticker_chunk) {
                std::string target{ep::k_ticker};
                for (std::size_t i = at; i < codes.size() && i < at + state_->flavour.ticker_chunk; ++i) {
                    target += i == at ? "" : ",";
                    target += codes[i];
                }
                targets.push_back(std::move(target));
            }
        }
        std::unordered_map<std::string, std::string> prices;
        static thread_local const auto rows = std::make_unique<TickerBuffer>();
        for (const auto& target : targets) {
            const auto body = get(target);
            if (!body.has_value()) {
                return std::unexpected{body.error()};
            }
            const auto tickers = parse_tickers(*body, *rows);
            if (!tickers.has_value()) {
                return std::unexpected{
                    exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(tickers.error()))};
            }
            for (const auto& row : *tickers) {
                prices.emplace(std::string{row.market}, std::string{row.trade_price});
            }
        }
        return prices;
    }

    std::shared_ptr<VenueState> state_;
    net::RestClient rest_;
};

} // namespace

std::unique_ptr<exec::InstrumentSource> make_instrument_source(std::shared_ptr<VenueState> state) {
    return std::make_unique<UpbitInstruments>(std::move(state));
}

} // namespace raikiri::venues::upbit::internal
