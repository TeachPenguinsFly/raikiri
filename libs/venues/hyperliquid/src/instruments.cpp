#include <raikiri/config/secret.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/hyperliquid/catalogue.hpp>
#include <raikiri/venues/hyperliquid/errors.hpp>
#include <raikiri/venues/hyperliquid/exchange.hpp>
#include <raikiri/venues/hyperliquid/map.hpp>
#include <raikiri/venues/hyperliquid/rest.hpp>
#include <raikiri/venues/hyperliquid/ws.hpp>

#include <array>
#include <charconv>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "clients.hpp"

namespace raikiri::venues::hyperliquid::detail {

namespace {

// The spot catalogue is 326 pairs plus every token behind them in one body, so
// the read that loads it gets more room than a reconciliation pull.
constexpr std::uint64_t k_catalogue_timeout_ms = 20'000;

class HyperliquidInstruments final : public exec::InstrumentSource {
public:
    explicit HyperliquidInstruments(std::shared_ptr<VenueState> state)
        : state_{std::move(state)},
          rest_{state_->io,
                net::RestConfig{.host = std::string{rest_host(state_->testnet)}, .port = "443"}} {
        rest_.set_admit(exec::market_data_gate(state_->budget, *state_->clock));
    }

    std::expected<std::vector<Instrument>, exec::VenueError> fetch_all(MarketType market) override {
        // Decided before a socket opens: asking for a market this venue does
        // not list costs nothing.
        switch (market) {
        case MarketType::Perp:
            return load_perp();
        case MarketType::Spot:
            return load_spot();
        case MarketType::Future:
            return std::unexpected{exec::not_implemented("hyperliquid dated futures catalogue")};
        case MarketType::Option:
            return std::unexpected{exec::not_implemented("hyperliquid options catalogue")};
        }
        return std::unexpected{exec::not_implemented("hyperliquid: unknown market")};
    }

private:
    std::expected<std::vector<Instrument>, exec::VenueError> load_perp() {
        // No clock sync anywhere in this adapter: /info is unauthenticated and
        // the venue publishes no server-time read.
        rest_.start();
        const auto body = read_info(*state_, rest_, RequestBuilder::meta(), k_catalogue_timeout_ms);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        static thread_local const auto storage_owner = std::make_unique<AssetBuffer>();
        auto& storage = *storage_owner;
        const auto wire = parse_meta(*body, storage);
        if (!wire.has_value()) {
            return std::unexpected{unreadable(wire.error())};
        }

        std::vector<Instrument> loaded;
        loaded.reserve(wire->size());
        state_->instruments.reserve(wire->size());
        for (const auto& entry : *wire) {
            auto instrument = map_instrument(entry, MarketType::Perp);
            if (!instrument.has_value()) {
                continue; // delisted, or an underivable step
            }
            // The row's position in `universe` IS the asset id. Counting the
            // rows we kept shifts every asset after a delisted one.
            keep(std::move(*instrument), entry.index, loaded);
        }
        if (auto dexes = load_dexes(loaded); !dexes.has_value()) {
            return std::unexpected{dexes.error()};
        }
        return loaded;
    }

    std::expected<void, exec::VenueError> load_dexes(std::vector<Instrument>& loaded) {
        std::vector<std::string> names;
        for (const auto& dex : state_->dexes) {
            if (dex != "all") {
                names.push_back(dex);
                continue;
            }
            const auto body =
                read_info(*state_, rest_, RequestBuilder::perp_dexs(), k_catalogue_timeout_ms);
            if (!body.has_value()) {
                return std::unexpected{body.error()};
            }
            PerpDexBuffer listed;
            const auto wire = parse_perp_dexs(*body, listed);
            if (!wire.has_value()) {
                return std::unexpected{unreadable(wire.error())};
            }
            names.insert(names.end(), wire->begin(), wire->end());
        }

        std::string categories_body;
        auto categories_storage = std::make_unique<PerpCategoryBuffer>();
        std::span<const WireCategory> categories;
        if (!names.empty()) {
            if (const auto body = read_info(
                    *state_, rest_, RequestBuilder::perp_categories(), k_catalogue_timeout_ms);
                body.has_value()) {
                categories_body.assign(*body);
                if (const auto rows = parse_perp_categories(categories_body, *categories_storage);
                    rows.has_value()) {
                    categories = *rows;
                }
            }
        }

        std::optional<std::string> spot_meta;
        auto storage = std::make_unique<AssetBuffer>();
        std::string request_body;
        for (const auto& dex : names) {
            const auto body = read_info(
                *state_, rest_, RequestBuilder::meta(dex, request_body), k_catalogue_timeout_ms);
            if (!body.has_value()) {
                return std::unexpected{body.error()};
            }
            const auto wire = parse_meta(*body, *storage);
            if (!wire.has_value()) {
                return std::unexpected{unreadable(wire.error())};
            }
            std::string_view collateral{"USDC"};
            if (storage->collateral_token != 0) {
                if (!spot_meta.has_value()) {
                    auto spot = read_info(
                        *state_, rest_, RequestBuilder::spot_meta(), k_catalogue_timeout_ms);
                    if (!spot.has_value()) {
                        return std::unexpected{spot.error()};
                    }
                    spot_meta = std::string{*spot};
                }
                const auto token = parse_token_name(*spot_meta, storage->collateral_token);
                if (!token.has_value()) {
                    continue;
                }
                collateral = *token;
            }
            for (const auto& entry : *wire) {
                if (auto instrument = map_dex_instrument(entry, collateral);
                    instrument.has_value()) {
                    std::visit(
                        [&](auto& row) {
                            row.base.asset_class = dex_asset_class(entry.name, categories);
                        },
                        *instrument);
                    keep(std::move(*instrument), std::nullopt, loaded);
                }
            }
        }
        return {};
    }

    std::expected<std::vector<Instrument>, exec::VenueError> load_spot() {
        rest_.start();
        const auto body =
            read_info(*state_, rest_, RequestBuilder::spot_meta(), k_catalogue_timeout_ms);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        auto storage = std::make_unique<SpotAssetBuffer>();
        const auto wire = parse_spot_meta(*body, *storage);
        if (!wire.has_value()) {
            return std::unexpected{unreadable(wire.error())};
        }

        std::vector<Instrument> loaded;
        loaded.reserve(wire->size());
        state_->instruments.reserve(state_->instruments.size() + wire->size());
        for (const auto& entry : *wire) {
            auto instrument = map_spot_instrument(entry);
            if (!instrument.has_value()) {
                continue;
            }
            // A DIFFERENT index space: pair 107 is asset 10107, while asset
            // 107 is an unrelated perpetual. Getting it wrong trades that.
            keep(std::move(*instrument), spot_asset_id(entry.index), loaded);
        }
        return loaded;
    }

    // No asset id for a HIP-3 row, so the order path refuses it: its /exchange
    // index is offset per dex and nothing here trades one.
    void keep(Instrument instrument,
              std::optional<std::int64_t> asset,
              std::vector<Instrument>& loaded) {
        const auto id = state_->instruments.add(std::move(instrument));
        if (!id.has_value()) {
            return; // duplicate or table full: reported by count, not by throw
        }
        if (asset.has_value()) {
            state_->asset_ids.emplace(id->packed, *asset);
        }
        // The table rewrote the id to the slot that indexes it.
        loaded.push_back(*state_->instruments.find(*id));
    }

    std::shared_ptr<VenueState> state_;
    net::RestClient rest_;
};

} // namespace

std::unique_ptr<exec::InstrumentSource> make_instrument_source(std::shared_ptr<VenueState> state) {
    return std::make_unique<HyperliquidInstruments>(std::move(state));
}

} // namespace raikiri::venues::hyperliquid::detail
