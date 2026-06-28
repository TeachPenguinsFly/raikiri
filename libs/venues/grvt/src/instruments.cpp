#include <raikiri/net/rest_await.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/venues/grvt/endpoints.hpp>
#include <raikiri/venues/grvt/map.hpp>
#include <raikiri/venues/grvt/wire.hpp>

#include <memory>
#include <string>
#include <vector>

#include "clients.hpp"

namespace raikiri::venues::grvt::detail {

namespace {

constexpr std::uint64_t k_catalogue_timeout_ms = 20'000;

class GrvtInstruments final : public exec::InstrumentSource {
public:
    explicit GrvtInstruments(std::shared_ptr<VenueState> state)
        : state_{std::move(state)},
          rest_{state_->io, net::RestConfig{.host = std::string{ep::host(state_->testnet)}}} {
        rest_.set_admit(exec::market_data_gate(state_->budget, *state_->clock));
    }

    std::expected<std::vector<Instrument>, exec::VenueError> fetch_all(MarketType market) override {
        if (market != MarketType::Perp) {
            return std::unexpected{exec::not_implemented("grvt lists perpetuals only")};
        }
        rest_.start();
        const net::RestRequest request{.method = net::HttpMethod::Post,
                                       .target = ep::k_all_instruments,
                                       .body = ep::k_all_instruments_body};
        const auto response =
            net::await_response(state_->io, *state_->clock, rest_, request, k_catalogue_timeout_ms);
        if (!response.has_value()) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Connectivity, 0, "catalogue: no response before deadline")};
        }
        if (response->transport_failed()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Connectivity, 0, response->error)};
        }
        if (response->status != 200) {
            const auto err = parse_rest_error(response->body);
            const auto cls = response->status == 429 ? exec::ErrorClass::RateLimited
                                                     : exec::ErrorClass::Terminal;
            return std::unexpected{exec::VenueError::make(
                cls,
                err.has_value() ? err->code : response->status,
                err.has_value() ? err->message : std::string_view{"catalogue refused"})};
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
        for (const auto& row : *wire) {
            const auto instrument = map_instrument(row, market);
            if (!instrument.has_value()) {
                continue;
            }
            const auto id = state_->instruments.add(*instrument);
            if (!id.has_value()) {
                continue;
            }
            loaded.push_back(*state_->instruments.find(*id));
        }
        return loaded;
    }

private:
    std::shared_ptr<VenueState> state_;
    net::RestClient rest_;
};

} // namespace

std::unique_ptr<exec::InstrumentSource> make_instrument_source(std::shared_ptr<VenueState> state) {
    return std::make_unique<GrvtInstruments>(std::move(state));
}

} // namespace raikiri::venues::grvt::detail
