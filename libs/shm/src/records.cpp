#include <raikiri/common/platform.hpp>
#include <raikiri/shm/records.hpp>

namespace raikiri::shm {

std::expected<GatewayChannels, SegError> create_gateway_channels(SegmentOwner& owner) {
    GatewayChannels ch;
    ch.catalogue = owner.create_channel<CatalogueTable>("catalogue");
    ch.bbo = owner.create_channel<BboTable>("bbo");
    ch.depth = owner.create_channel<DepthTable>("depth");
    ch.funding = owner.create_channel<FundingTable>("funding");
    ch.greeks = owner.create_channel<GreeksTable>("greeks");
    ch.ticker_stats = owner.create_channel<TickerStatsTable>("ticker_stats");
    ch.md_events = owner.create_channel<MdRing>("md_events");
    ch.exec_events = owner.create_channel<ExecRing>("exec_events");
    ch.funding_charges = owner.create_channel<LedgerEntryRing>("funding_charges");
    ch.fills = owner.create_channel<FillRing>("fills");
    ch.positions = owner.create_channel<PositionTable>("positions");
    ch.open_orders = owner.create_channel<OpenOrderTable>("open_orders");
    ch.balances = owner.create_channel<BalanceTable>("balances");
    ch.cmd_create = owner.create_channel<CmdRing>("cmd_create");
    ch.cmd_cancel = owner.create_channel<CmdRing>("cmd_cancel");
    ch.order_intents = owner.create_channel<CmdRing>("order_intents");
    ch.metrics = owner.create_channel<VenueMetricsCell>("metrics");
    if (ch.catalogue == nullptr || ch.bbo == nullptr || ch.depth == nullptr || ch.funding == nullptr ||
        ch.md_events == nullptr || ch.exec_events == nullptr || ch.funding_charges == nullptr || ch.fills == nullptr ||
        ch.open_orders == nullptr || ch.greeks == nullptr || ch.ticker_stats == nullptr || ch.positions == nullptr ||
        ch.balances == nullptr || ch.cmd_create == nullptr || ch.cmd_cancel == nullptr || ch.order_intents == nullptr ||
        ch.metrics == nullptr) {
        return std::unexpected{SegError::TooSmall}; // segment sized too small
    }
    return ch;
}

std::expected<GatewayChannels, SegError> open_gateway_channels(SegmentClient& client) {
    const auto catalogue = client.open_channel<CatalogueTable>("catalogue");
    if (!catalogue) {
        return std::unexpected{catalogue.error()};
    }
    const auto bbo = client.open_channel<BboTable>("bbo");
    if (!bbo) {
        return std::unexpected{bbo.error()};
    }
    const auto depth = client.open_channel<DepthTable>("depth");
    if (!depth) {
        return std::unexpected{depth.error()};
    }
    const auto funding = client.open_channel<FundingTable>("funding");
    if (!funding) {
        return std::unexpected{funding.error()};
    }
    const auto greeks = client.open_channel<GreeksTable>("greeks");
    if (!greeks) {
        return std::unexpected{greeks.error()};
    }
    const auto ticker = client.open_channel<TickerStatsTable>("ticker_stats");
    if (!ticker) {
        return std::unexpected{ticker.error()};
    }
    const auto md = client.open_channel<MdRing>("md_events");
    if (!md) {
        return std::unexpected{md.error()};
    }
    const auto exec = client.open_channel<ExecRing>("exec_events");
    if (!exec) {
        return std::unexpected{exec.error()};
    }
    const auto charges = client.open_channel<LedgerEntryRing>("funding_charges");
    if (!charges) {
        return std::unexpected{charges.error()};
    }
    const auto fills = client.open_channel<FillRing>("fills");
    if (!fills) {
        return std::unexpected{fills.error()};
    }
    const auto positions = client.open_channel<PositionTable>("positions");
    if (!positions) {
        return std::unexpected{positions.error()};
    }
    const auto open_orders = client.open_channel<OpenOrderTable>("open_orders");
    if (!open_orders) {
        return std::unexpected{open_orders.error()};
    }
    const auto balances = client.open_channel<BalanceTable>("balances");
    if (!balances) {
        return std::unexpected{balances.error()};
    }
    const auto create = client.open_channel<CmdRing>("cmd_create");
    if (!create) {
        return std::unexpected{create.error()};
    }
    const auto cancel = client.open_channel<CmdRing>("cmd_cancel");
    if (!cancel) {
        return std::unexpected{cancel.error()};
    }
    const auto intents = client.open_channel<CmdRing>("order_intents");
    if (!intents) {
        return std::unexpected{intents.error()};
    }
    const auto metrics = client.open_channel<VenueMetricsCell>("metrics");
    if (!metrics) {
        return std::unexpected{metrics.error()};
    }
    // named so a field added to GatewayChannels cannot shift the others
    return GatewayChannels{.catalogue = *catalogue,
                           .bbo = *bbo,
                           .depth = *depth,
                           .funding = *funding,
                           .greeks = *greeks,
                           .ticker_stats = *ticker,
                           .md_events = *md,
                           .exec_events = *exec,
                           .funding_charges = *charges,
                           .fills = *fills,
                           .positions = *positions,
                           .open_orders = *open_orders,
                           .balances = *balances,
                           .cmd_create = *create,
                           .cmd_cancel = *cancel,
                           .order_intents = *intents,
                           .metrics = *metrics};
}

std::string gateway_segment_path(AccountId account) {
    char buf[16];
    const auto name = format_account(account, buf, sizeof buf);
    return platform::runtime_dir() + "/shm/gw." + std::string{name};
}

} // namespace raikiri::shm
