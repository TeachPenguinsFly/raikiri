#include <raikiri/log/log.hpp>
#include <raikiri/net/rest_await.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/venues/phemex/catalogue.hpp>
#include <raikiri/venues/phemex/endpoints.hpp>
#include <raikiri/venues/phemex/map.hpp>

#include <memory>
#include <string>
#include <vector>

#include "clients.hpp"

namespace raikiri::venues::phemex::internal {

namespace {

constexpr std::uint32_t k_catalogue_timeout_ms = 15'000;

class PhemexInstruments final : public exec::InstrumentSource {
public:
    explicit PhemexInstruments(std::shared_ptr<VenueState> state)
        : state_{std::move(state)},
          rest_{state_->io,
                net::RestConfig{.host = std::string{ep::rest_host(state_->testnet)},
                                .port = "443",
                                .pool_size = 1,
                                .timeout_ms = k_catalogue_timeout_ms}} {
        rest_.set_admit(exec::market_data_gate(state_->budget, *state_->clock));
    }

    std::expected<std::vector<Instrument>, exec::VenueError> fetch_all(MarketType market) override {
        rest_.start();
        const net::RestRequest request{
            .method = net::HttpMethod::Get, .target = ep::k_products, .body = {}};
        const auto response = net::await_response(
            state_->io, *state_->clock, rest_, request, rest_.max_wait_ms() + 1'000);
        if (!response.has_value()) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Connectivity, 0, "catalogue: no response before deadline")};
        }
        if (response->transport_failed()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Connectivity, 0, response->error)};
        }
        if (response->status == 429) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::RateLimited, response->status, "catalogue: rate limited")};
        }
        if (response->status != 200) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal, response->status, "catalogue: unexpected status")};
        }

        const auto storage = std::make_unique<CatalogueBuffer>();
        const auto wire = parse_products(response->body, *storage);
        if (!wire.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(wire.error()))};
        }
        auto mapped = map_catalogue(*wire, market);

        state_->market = market;
        state_->instruments.reserve(mapped.listed.size());
        std::vector<Instrument> loaded;
        loaded.reserve(mapped.listed.size());
        std::size_t inverse = 0;
        for (auto& row : mapped.listed) {
            const auto id = state_->instruments.add(row.instrument);
            if (!id.has_value()) {
                ++mapped.refused;
                continue;
            }
            state_->listings.emplace(id->packed, row.listing);
            const auto* added = state_->instruments.find(*id);
            if (base_of(*added).kind == ContractKind::Inverse) {
                ++inverse;
            }
            loaded.push_back(*added);
        }
        if (::raikiri::log::logger() != nullptr) {
            RLOG_INFO("phemex: {} {} instrument(s) loaded ({} linear, {} inverse); {} listed "
                      "row(s) refused, {} malformed row(s) skipped",
                      loaded.size(),
                      to_string(market),
                      loaded.size() - inverse,
                      inverse,
                      mapped.refused,
                      wire->malformed);
        }
        return loaded;
    }

private:
    std::shared_ptr<VenueState> state_;
    net::RestClient rest_;
};

} // namespace

std::unique_ptr<exec::InstrumentSource> make_instrument_source(std::shared_ptr<VenueState> state) {
    return std::make_unique<PhemexInstruments>(std::move(state));
}

} // namespace raikiri::venues::phemex::internal
