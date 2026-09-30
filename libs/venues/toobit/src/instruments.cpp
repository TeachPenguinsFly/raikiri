#include <raikiri/log/log.hpp>
#include <raikiri/net/rest_await.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/venues/toobit/endpoints.hpp>
#include <raikiri/venues/toobit/map.hpp>
#include <raikiri/venues/toobit/wire.hpp>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "clients.hpp"

namespace raikiri::venues::toobit::internal {

namespace {

constexpr std::uint32_t k_catalogue_timeout_ms = 10'000;

class ToobitInstruments final : public exec::InstrumentSource {
public:
    explicit ToobitInstruments(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, rest_{state_->io,
                                          net::RestConfig{.host = std::string{ep::k_rest_host},
                                                          .port = "443",
                                                          .pool_size = 1,
                                                          .timeout_ms = k_catalogue_timeout_ms}} {
        rest_.set_admit(exec::market_data_gate(state_->budget, *state_->clock));
    }

    std::expected<std::vector<Instrument>, exec::VenueError> fetch_all(MarketType market) override {
        rest_.start();
        const auto body = pull(ep::k_exchange_info);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        static thread_local const auto storage = std::make_unique<MarketBuffer>();
        const auto rows = parse_exchange_info(*body, *storage);
        if (!rows.has_value()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(rows.error()))};
        }

        std::string funding_body;
        std::unordered_map<std::string_view, const WireFunding*> funding;
        static thread_local const auto funding_storage = std::make_unique<FundingBuffer>();
        if (market == MarketType::Perp) {
            auto pulled = pull(ep::k_funding_rate);
            if (!pulled.has_value()) {
                return std::unexpected{pulled.error()};
            }
            funding_body = std::move(*pulled);
            const auto terms = parse_funding_rates(funding_body, *funding_storage);
            if (!terms.has_value()) {
                return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(terms.error()))};
            }
            for (const auto& row : *terms) {
                funding.emplace(row.symbol, &row);
            }
        }

        std::vector<Instrument> loaded;
        std::size_t listed = 0;
        for (const auto& row : *rows) {
            if (row.contract != (market == MarketType::Perp)) {
                continue;
            }
            ++listed;
            const auto terms = funding.find(row.symbol);
            const auto instrument = map_instrument(row, market, terms == funding.end() ? nullptr : terms->second);
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
            RLOG_INFO("toobit: {} {} rows listed, {} tradable through the api loaded",
                      listed,
                      to_string(market),
                      loaded.size());
        }
        return loaded;
    }

private:
    std::expected<std::string, exec::VenueError> pull(std::string_view target) {
        net::RestRequest request{};
        request.method = net::HttpMethod::Get;
        request.target = target;
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
        return response->body;
    }

    std::shared_ptr<VenueState> state_;
    net::RestClient rest_;
};

} // namespace

std::unique_ptr<exec::InstrumentSource> make_instrument_source(std::shared_ptr<VenueState> state) {
    return std::make_unique<ToobitInstruments>(std::move(state));
}

} // namespace raikiri::venues::toobit::internal
