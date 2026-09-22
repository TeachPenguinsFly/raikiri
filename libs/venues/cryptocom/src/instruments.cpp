#include <raikiri/log/log.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/venues/cryptocom/catalogue.hpp>
#include <raikiri/venues/cryptocom/endpoints.hpp>
#include <raikiri/venues/cryptocom/envelope.hpp>
#include <raikiri/venues/cryptocom/map.hpp>

#include <memory>
#include <string>
#include <vector>

#include "clients.hpp"

namespace raikiri::venues::cryptocom::internal {

namespace {

class CryptoComInstruments final : public exec::InstrumentSource {
public:
    explicit CryptoComInstruments(std::shared_ptr<VenueState> state)
        : state_{std::move(state)},
          // Both legs of max_wait_ms must fit inside the 20 s await below.
          rest_{state_->io,
                net::RestConfig{.host = std::string{ep::rest_host(state_->testnet)},
                                .port = "443",
                                .timeout_ms = 9'000}} {
        rest_.set_admit(exec::market_data_gate(state_->budget, *state_->clock));
    }

    std::expected<std::vector<Instrument>, exec::VenueError> fetch_all(MarketType market) override {
        rest_.start();
        const net::RestRequest request{
            .method = net::HttpMethod::Get, .target = ep::k_instruments, .body = {}};
        const auto response = await_response(*state_, rest_, request, 20'000);
        if (!response.has_value()) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Connectivity, 0, "catalogue: no response before deadline")};
        }
        if (response->transport_failed()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Connectivity, 0, response->error)};
        }
        const auto status = parse_rest_status(response->body);
        if (!status.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(status.error()))};
        }
        if (status->code != k_ok) {
            return std::unexpected{
                exec::VenueError::make(classify(status->code), status->code, status->message)};
        }
        // A 404 body carries no `code`, which reads as success above.
        if (response->status != 200) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal, response->status, "catalogue: HTTP error")};
        }

        static thread_local const auto storage_owner = std::make_unique<InstrumentBuffer>();
        const auto wire = parse_instruments(response->body, *storage_owner);
        if (!wire.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(wire.error()))};
        }

        std::vector<Instrument> loaded;
        loaded.reserve(wire->size());
        state_->instruments.reserve(wire->size());
        std::size_t listed = 0;
        for (const auto& row : *wire) {
            const bool this_market =
                (market == MarketType::Spot && row.inst_type == "CCY_PAIR") ||
                (market == MarketType::Perp && row.inst_type == "PERPETUAL_SWAP");
            listed += this_market ? 1 : 0;
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
        if (loaded.size() != listed && ::raikiri::log::logger() != nullptr) {
            RLOG_INFO("cryptocom: {} of {} listed {} instruments loaded; the rest are not "
                      "tradable or quote a step finer than 1e-9",
                      loaded.size(),
                      listed,
                      to_string(market));
        }
        return loaded;
    }

private:
    std::shared_ptr<VenueState> state_;
    net::RestClient rest_;
};

} // namespace

std::unique_ptr<exec::InstrumentSource> make_instrument_source(std::shared_ptr<VenueState> state) {
    return std::make_unique<CryptoComInstruments>(std::move(state));
}

} // namespace raikiri::venues::cryptocom::internal
