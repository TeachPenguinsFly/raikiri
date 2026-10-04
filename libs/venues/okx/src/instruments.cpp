#include <raikiri/config/secret.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/log/log.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/okx/catalogue.hpp>
#include <raikiri/venues/okx/envelope.hpp>
#include <raikiri/venues/okx/errors.hpp>
#include <raikiri/venues/okx/map.hpp>
#include <raikiri/venues/okx/rest.hpp>
#include <raikiri/venues/okx/ws.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "clients.hpp"

namespace raikiri::venues::okx::internal {

namespace {

// gatewayd's shm tables hold this many instruments; the furthest expiries are cut first
constexpr std::size_t k_catalogue_cap = 4096;

[[nodiscard]] bool wanted(std::string_view families, std::string_view family) {
    if (families.empty()) {
        return true;
    }
    while (!families.empty()) {
        const auto comma = families.find(',');
        if (families.substr(0, comma) == family) {
            return true;
        }
        families = comma == std::string_view::npos ? std::string_view{} : families.substr(comma + 1);
    }
    return false;
}

class OkxInstruments final : public exec::InstrumentSource {
public:
    explicit OkxInstruments(std::shared_ptr<VenueState> state)
        : state_{std::move(state)},
          rest_{state_->io, net::RestConfig{.host = std::string{rest_host()}, .port = "443"}} {
        rest_.set_admit(exec::market_data_gate(state_->budget, *state_->clock));
    }

    std::expected<std::vector<Instrument>, exec::VenueError> fetch_all(MarketType market) override {
        rest_.start();
        if (market == MarketType::Option) {
            return fetch_options();
        }

        const auto inst_type = market == MarketType::Spot ? "SPOT" : "SWAP";
        const auto response = fetch(state_->builder.instruments(inst_type));
        if (!response.has_value()) {
            return std::unexpected{response.error()};
        }

        static thread_local const auto storage_owner = std::make_unique<InstrumentBuffer>();

        auto& storage = *storage_owner;
        const auto wire = parse_instruments(response->body, storage);
        if (!wire.has_value()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(wire.error()))};
        }

        std::vector<Instrument> loaded;
        loaded.reserve(wire->size());
        state_->instruments.reserve(wire->size());
        for (const auto& entry : *wire) {
            auto instrument = map_instrument(entry, market);
            if (!instrument.has_value()) {
                continue; // not live, not this market, or missing a step
            }
            const auto id = state_->instruments.add(*instrument);
            if (!id.has_value()) {
                continue; // duplicate or table full: reported by count, not by throw
            }
            if (entry.inst_id_code != 0) {
                state_->inst_id_codes[*id] = entry.inst_id_code;
            }
            loaded.push_back(*state_->instruments.find(*id));
        }
        return loaded;
    }

private:
    std::expected<net::RestResponse, exec::VenueError> fetch(const net::RestRequest& request) {
        auto response = await_response(*state_, rest_, request, 20'000);
        if (!response.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Connectivity, 0, "catalogue: no response before deadline")};
        }
        if (response->transport_failed()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Connectivity, 0, response->error)};
        }
        const auto envelope = parse_rest_envelope(response->body);
        if (!envelope.has_value()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(envelope.error()))};
        }
        if (envelope->code != k_ok) {
            return std::unexpected{exec::VenueError::make(classify(envelope->code), envelope->code, envelope->msg)};
        }
        return std::move(*response);
    }

    // every underlying's families, coin-margined first so a chain keeps them over a _UM twin
    std::expected<std::vector<Instrument>, exec::VenueError> fetch_options() {
        const auto listing = fetch(state_->builder.option_underlyings());
        if (!listing.has_value()) {
            return std::unexpected{listing.error()};
        }
        UnderlyingBuffer names_storage;
        const auto names = parse_underlyings(listing->body, names_storage);
        if (!names.has_value()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(names.error()))};
        }
        std::vector<std::string> ulys{names->begin(), names->end()};

        static thread_local const auto storage_owner = std::make_unique<InstrumentBuffer>();
        std::vector<Instrument> mapped;
        std::size_t listed = 0;
        std::size_t too_long = 0;
        for (const auto& uly : ulys) {
            const auto response = fetch(state_->builder.option_instruments(uly));
            if (!response.has_value()) {
                return std::unexpected{response.error()};
            }
            const auto wire = parse_instruments(response->body, *storage_owner);
            if (!wire.has_value()) {
                return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(wire.error()))};
            }
            listed += wire->size();
            for (const auto& entry : *wire) {
                if (!wanted(state_->families, entry.inst_family)) {
                    continue;
                }
                // a _UM name with a five-digit strike is 25 or 26 bytes, past what SymbolText holds
                if (!SymbolText::from(entry.inst_id).has_value()) {
                    ++too_long;
                    continue;
                }
                if (auto instrument = map_instrument(entry, MarketType::Option); instrument.has_value()) {
                    mapped.push_back(*instrument);
                }
            }
        }

        const auto order = [](const Instrument& instrument) {
            const auto& option = std::get<OptionInstrument>(instrument);
            return std::pair{option.expiry.ns, option.base.kind == ContractKind::Inverse ? 0 : 1};
        };
        std::ranges::stable_sort(mapped, {}, order);
        const auto cut = mapped.size() > k_catalogue_cap ? mapped.size() - k_catalogue_cap : 0;
        mapped.resize(mapped.size() - cut);

        std::vector<Instrument> loaded;
        loaded.reserve(mapped.size());
        state_->instruments.reserve(mapped.size());
        for (const auto& instrument : mapped) {
            if (const auto id = state_->instruments.add(instrument); id.has_value()) {
                loaded.push_back(*state_->instruments.find(*id));
            }
        }
        if (::raikiri::log::logger() == nullptr) {
            return loaded;
        }
        RLOG_INFO("okx: {} option rows listed across {} underlyings, {} loaded, {} named past the symbol width, {} "
                  "furthest-dated cut at the table cap",
                  listed,
                  ulys.size(),
                  loaded.size(),
                  too_long,
                  cut);
        return loaded;
    }

    std::shared_ptr<VenueState> state_;
    net::RestClient rest_;
};

} // namespace

std::unique_ptr<exec::InstrumentSource> make_instrument_source(std::shared_ptr<VenueState> state) {
    return std::make_unique<OkxInstruments>(std::move(state));
}

} // namespace raikiri::venues::okx::internal
