#include <raikiri/log/log.hpp>
#include <raikiri/net/rest_await.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/venues/apex/endpoints.hpp>
#include <raikiri/venues/apex/map.hpp>
#include <raikiri/venues/apex/wire.hpp>

#include <memory>
#include <string>
#include <vector>

#include "clients.hpp"

namespace raikiri::venues::apex::detail {

namespace {

constexpr std::uint32_t k_catalogue_timeout_ms = 10'000;

class ApexInstruments final : public exec::InstrumentSource {
public:
    explicit ApexInstruments(std::shared_ptr<VenueState> state)
        : state_{std::move(state)},
          rest_{state_->io,
                net::RestConfig{.host = std::string{ep::rest_host(state_->testnet)},
                                .port = "443",
                                .pool_size = 1,
                                .timeout_ms = k_catalogue_timeout_ms}} {}

    std::expected<std::vector<Instrument>, exec::VenueError> fetch_all(MarketType market) override {
        rest_.start();
        const net::RestRequest request{
            .method = net::HttpMethod::Get, .target = ep::k_symbols, .body = {}};
        const auto response = net::await_response(
            state_->io, *state_->clock, rest_, request, 2ULL * k_catalogue_timeout_ms + 1'000);
        if (!response.has_value()) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Connectivity, 0, "catalogue: no response before deadline")};
        }
        if (response->transport_failed()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Connectivity, 0, response->error)};
        }
        if (response->status == ep::k_ban_status || response->status == 429) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::RateLimited, response->status, "catalogue: rate limited")};
        }
        if (response->status != 200) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal, response->status, "catalogue: unexpected status")};
        }

        static thread_local const auto storage = std::make_unique<ContractBuffer>();
        const auto wire = parse_symbols(response->body, *storage);
        if (!wire.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(wire.error()))};
        }

        std::vector<Instrument> loaded;
        loaded.reserve(wire->size());
        state_->instruments.reserve(wire->size());
        std::size_t disabled = 0;
        for (const auto& row : *wire) {
            if (!row.enable_trade) {
                ++disabled;
                continue;
            }
            auto instrument = map_instrument(row, market);
            if (!instrument.has_value()) {
                continue;
            }
            const auto id = state_->instruments.add(*instrument);
            if (!id.has_value()) {
                continue;
            }
            loaded.push_back(*state_->instruments.find(*id));
        }
        if (::raikiri::log::logger() != nullptr) {
            RLOG_INFO("apex: {} of {} listed perps loaded, {} disabled for trading and never "
                      "subscribed",
                      loaded.size(),
                      wire->size(),
                      disabled);
        }
        return loaded;
    }

private:
    std::shared_ptr<VenueState> state_;
    net::RestClient rest_;
};

} // namespace

std::unique_ptr<exec::InstrumentSource> make_instrument_source(std::shared_ptr<VenueState> state) {
    return std::make_unique<ApexInstruments>(std::move(state));
}

} // namespace raikiri::venues::apex::detail
