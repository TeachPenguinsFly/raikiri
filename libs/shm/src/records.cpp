#include <raikiri/common/platform.hpp>
#include <raikiri/shm/records.hpp>

namespace raikiri::shm {

std::expected<GatewayChannels, SegError> create_gateway_channels(SegmentOwner& owner) {
    GatewayChannels ch;
    ch.catalogue = owner.create_channel<CatalogueTable>("catalogue");
    ch.bbo = owner.create_channel<BboTable>("bbo");
    ch.depth = owner.create_channel<DepthTable>("depth");
    ch.funding = owner.create_channel<FundingTable>("funding");
    ch.md_events = owner.create_channel<MdRing>("md_events");
    ch.exec_events = owner.create_channel<ExecRing>("exec_events");
    ch.positions = owner.create_channel<PositionTable>("positions");
    ch.cmd_create = owner.create_channel<CmdRing>("cmd_create");
    ch.cmd_cancel = owner.create_channel<CmdRing>("cmd_cancel");
    if (ch.catalogue == nullptr || ch.bbo == nullptr || ch.depth == nullptr ||
        ch.funding == nullptr || ch.md_events == nullptr || ch.exec_events == nullptr ||
        ch.positions == nullptr || ch.cmd_create == nullptr ||
        ch.cmd_cancel == nullptr) {
        return std::unexpected{SegError::TooSmall}; // segment sized too small: misconfiguration
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
    const auto md = client.open_channel<MdRing>("md_events");
    if (!md) {
        return std::unexpected{md.error()};
    }
    const auto exec = client.open_channel<ExecRing>("exec_events");
    if (!exec) {
        return std::unexpected{exec.error()};
    }
    const auto positions = client.open_channel<PositionTable>("positions");
    if (!positions) {
        return std::unexpected{positions.error()};
    }
    const auto create = client.open_channel<CmdRing>("cmd_create");
    if (!create) {
        return std::unexpected{create.error()};
    }
    const auto cancel = client.open_channel<CmdRing>("cmd_cancel");
    if (!cancel) {
        return std::unexpected{cancel.error()};
    }
    return GatewayChannels{
        *catalogue, *bbo, *depth, *funding, *md, *exec, *positions, *create, *cancel};
}

std::string gateway_segment_path(std::string_view venue_name) {
    return platform::runtime_dir() + "/shm/gw." + std::string{venue_name};
}

} // namespace raikiri::shm
