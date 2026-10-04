#include <raikiri/config/secret.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/deribit/book.hpp>
#include <raikiri/venues/deribit/envelope.hpp>
#include <raikiri/venues/deribit/errors.hpp>
#include <raikiri/venues/deribit/map.hpp>
#include <raikiri/venues/deribit/rest.hpp>
#include <raikiri/venues/deribit/ws.hpp>

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "clients.hpp"

namespace raikiri::venues::deribit::internal {

namespace {

class DeribitInstruments final : public exec::InstrumentSource {
public:
    explicit DeribitInstruments(std::shared_ptr<VenueState> state)
        : state_{std::move(state)},
          rest_{state_->io, net::RestConfig{.host = std::string{rest_host(state_->testnet)}, .port = "443"}} {
        rest_.set_admit(exec::market_data_gate(state_->budget, *state_->clock));
    }

    std::expected<std::vector<Instrument>, exec::VenueError> fetch_all(MarketType market) override {
        std::string_view kind;
        switch (market) {
        case MarketType::Perp:
        case MarketType::Future:
            kind = "future";
            break;
        case MarketType::Option:
            kind = "option";
            break;
        case MarketType::Spot:
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "deribit spot is not modelled")};
        }

        rest_.start();

        std::vector<Instrument> mapped;
        for (const auto currency : k_currencies) {
            const auto response = await_response(*state_, rest_, state_->builder.instruments(currency, kind), 20'000);
            if (!response.has_value()) {
                return std::unexpected{exec::VenueError::make(
                    exec::ErrorClass::Connectivity, 0, "catalogue: no response before deadline")};
            }
            if (response->transport_failed()) {
                return std::unexpected{exec::VenueError::make(exec::ErrorClass::Connectivity, 0, response->error)};
            }
            const auto envelope = parse_response(response->body);
            if (!envelope.has_value()) {
                return std::unexpected{
                    exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(envelope.error()))};
            }
            if (envelope->failed()) {
                return std::unexpected{exec::VenueError::make(
                    classify(envelope->error_code), envelope->error_code, envelope->error_message)};
            }

            static thread_local const auto storage_owner = std::make_unique<InstrumentBuffer>();

            auto& storage = *storage_owner;
            const auto wire = parse_instruments(response->body, storage);
            if (!wire.has_value()) {
                return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(wire.error()))};
            }
            for (const auto& entry : *wire) {
                if (auto instrument = map_instrument(entry, market); instrument.has_value()) {
                    mapped.push_back(std::move(*instrument));
                }
            }
        }

        // nothing enters the table until every currency answered, so a retry after a timeout numbers from zero
        std::vector<Instrument> loaded;
        state_->instruments.reserve(state_->instruments.size() + mapped.size());
        for (auto& instrument : mapped) {
            if (const auto id = state_->instruments.add(std::move(instrument)); id.has_value()) {
                loaded.push_back(*state_->instruments.find(*id));
            }
        }
        return loaded;
    }

private:
    std::shared_ptr<VenueState> state_;
    net::RestClient rest_;
};

} // namespace

std::unique_ptr<exec::InstrumentSource> make_instrument_source(std::shared_ptr<VenueState> state) {
    return std::make_unique<DeribitInstruments>(std::move(state));
}

} // namespace raikiri::venues::deribit::internal
