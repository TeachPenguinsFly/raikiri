#include <raikiri/log/log.hpp>
#include <raikiri/net/rest_await.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/venues/derive/endpoints.hpp>
#include <raikiri/venues/derive/map.hpp>
#include <raikiri/venues/derive/wire.hpp>

#include <algorithm>
#include <format>
#include <memory>
#include <string>
#include <vector>

#include "clients.hpp"

namespace raikiri::venues::derive::internal {

namespace {

constexpr std::uint32_t k_catalogue_timeout_ms = 15'000;
constexpr std::uint32_t k_max_pages = 20;

[[nodiscard]] bool wanted(std::string_view underlyings, std::string_view base) {
    if (underlyings.empty()) {
        return true;
    }
    while (!underlyings.empty()) {
        const auto comma = underlyings.find(',');
        if (underlyings.substr(0, comma) == base) {
            return true;
        }
        underlyings = comma == std::string_view::npos ? std::string_view{} : underlyings.substr(comma + 1);
    }
    return false;
}

[[nodiscard]] UnixNanos expiry_of(const Instrument& instrument) noexcept {
    const auto* option = std::get_if<OptionInstrument>(&instrument);
    return option == nullptr ? UnixNanos{0} : option->expiry;
}

class DeriveInstruments final : public exec::InstrumentSource {
public:
    explicit DeriveInstruments(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, rest_{state_->io,
                                          net::RestConfig{.host = std::string{ep::k_host},
                                                          .port = "443",
                                                          .pool_size = 1,
                                                          .timeout_ms = k_catalogue_timeout_ms}} {
        rest_.set_admit(exec::market_data_gate(state_->budget, *state_->clock));
    }

    std::expected<std::vector<Instrument>, exec::VenueError> fetch_all(MarketType market) override {
        if (market != MarketType::Option && market != MarketType::Perp) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "derive lists options and perpetuals only")};
        }
        rest_.start();
        const auto now = state_->clock->wall();
        std::vector<Instrument> mapped;
        std::size_t listed = 0;
        std::uint32_t pages = 1;
        for (std::uint32_t page = 1; page <= pages && page <= k_max_pages; ++page) {
            const auto body = std::format(R"({{"instrument_type":"{}","expired":false,"page":{},"page_size":{}}})",
                                          market == MarketType::Option ? "option" : "perp",
                                          page,
                                          k_page_size);
            const net::RestRequest request{
                .method = net::HttpMethod::Post, .target = ep::k_all_instruments, .body = body};
            const auto response = net::await_response(
                state_->io, *state_->clock, rest_, request, 2ULL * k_catalogue_timeout_ms + 1'000);
            if (!response.has_value()) {
                return std::unexpected{exec::VenueError::make(
                    exec::ErrorClass::Connectivity, 0, "catalogue: no response before deadline")};
            }
            if (response->transport_failed()) {
                return std::unexpected{exec::VenueError::make(exec::ErrorClass::Connectivity, 0, response->error)};
            }
            if (const auto error = parse_rpc_error(response->body); error.has_value()) {
                return std::unexpected{exec::VenueError::make(error->code == -32000 ? exec::ErrorClass::RateLimited
                                                                                    : exec::ErrorClass::Terminal,
                                                              error->code,
                                                              error->message)};
            }
            if (response->status != 200) {
                return std::unexpected{
                    exec::VenueError::make(exec::ErrorClass::Terminal, response->status, "catalogue: refused")};
            }
            static thread_local const auto storage = std::make_unique<InstrumentBuffer>();
            const auto rows = parse_instruments(response->body, *storage);
            if (!rows.has_value()) {
                return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(rows.error()))};
            }
            pages = rows->num_pages;
            listed += rows->rows.size();
            for (const auto& row : rows->rows) {
                if (!wanted(state_->underlyings, row.base_currency)) {
                    continue;
                }
                if (auto instrument = map_instrument(row, market, now); instrument.has_value()) {
                    mapped.push_back(*instrument);
                }
            }
        }

        std::ranges::stable_sort(mapped, {}, expiry_of);
        const auto cut = mapped.size() > ep::k_catalogue_cap ? mapped.size() - ep::k_catalogue_cap : 0;
        mapped.resize(mapped.size() - cut);

        std::vector<Instrument> loaded;
        loaded.reserve(mapped.size());
        state_->instruments.reserve(mapped.size());
        for (const auto& instrument : mapped) {
            const auto id = state_->instruments.add(instrument);
            if (!id.has_value()) {
                continue;
            }
            if (state_->markets.size() <= id->symbol().raw) {
                state_->markets.resize(id->symbol().raw + 1);
            }
            loaded.push_back(*state_->instruments.find(*id));
        }
        if (::raikiri::log::logger() != nullptr) {
            RLOG_INFO("derive: {} {} rows listed, {} loaded as active, {} furthest-dated cut at the table cap",
                      listed,
                      to_string(market),
                      loaded.size(),
                      cut);
        }
        return loaded;
    }

private:
    std::shared_ptr<VenueState> state_;
    net::RestClient rest_;
};

} // namespace

std::unique_ptr<exec::InstrumentSource> make_instrument_source(std::shared_ptr<VenueState> state) {
    return std::make_unique<DeriveInstruments>(std::move(state));
}

} // namespace raikiri::venues::derive::internal
