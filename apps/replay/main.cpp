// replay — decode a journal directory into a human-readable stream.
//
//   replay <dir> [--prefix <name>] [--stats]
//
// Batch decoder: walks everything committed and exits (it does not tail).
// Doubles as the verification tool behind golden-replay tests: identical
// journals must produce byte-identical replay output.

#include <raikiri/domain/identifiers.hpp>
#include <raikiri/journal/reader.hpp>
#include <raikiri/log/log.hpp>
#include <raikiri/schema/codec.hpp>

#include <fmt/format.h>

#include <cstring>
#include <map>
#include <string>

namespace {

using msgs = raikiri_journal::schema::messages;
using raikiri::journal::FrameView;

std::string instrument_str(std::uint64_t packed) {
    const raikiri::InstrumentId id{packed};
    return fmt::format("{}:{}:{}",
                       raikiri::to_string(id.venue()),
                       raikiri::to_string(id.market()),
                       id.symbol().raw);
}

void print_frame(const FrameView& frame) {
    const auto head = fmt::format("#{:<8} t={} ", frame.seq, frame.ts_ns);
    if (frame.schema_id == raikiri::schema::message_id<msgs::BboMsg>()) {
        const auto m = raikiri::schema::view_msg<msgs::BboMsg>(frame.payload);
        fmt::print("{}BBO   {} bid={}x{} ask={}x{}\n",
                   head,
                   instrument_str(m.instrument().value()),
                   m.bid().value(),
                   m.bidQty().value(),
                   m.ask().value(),
                   m.askQty().value());
    } else if (frame.schema_id == raikiri::schema::message_id<msgs::TradeMsg>()) {
        const auto m = raikiri::schema::view_msg<msgs::TradeMsg>(frame.payload);
        fmt::print("{}TRADE {} px={} qty={} aggr={}\n",
                   head,
                   instrument_str(m.instrument().value()),
                   m.price().value(),
                   m.qty().value(),
                   m.aggressor() == raikiri_journal::types::sideEnum::Buy ? "buy" : "sell");
    } else if (frame.schema_id == raikiri::schema::message_id<msgs::OrderEventMsg>()) {
        const auto m = raikiri::schema::view_msg<msgs::OrderEventMsg>(frame.payload);
        fmt::print("{}OEVT  {} type={} coid={:x} px={} qty={} leaves={}\n",
                   head,
                   instrument_str(m.instrument().value()),
                   static_cast<int>(sbepp::to_underlying(m.eventType())),
                   m.clientOrderIdField().value(),
                   m.price().value(),
                   m.qty().value(),
                   m.leavesQty().value());
    } else if (frame.schema_id == raikiri::schema::message_id<msgs::OrderCmdMsg>()) {
        const auto m = raikiri::schema::view_msg<msgs::OrderCmdMsg>(frame.payload);
        fmt::print("{}OCMD  {} kind={} coid={:x} px={} qty={}\n",
                   head,
                   instrument_str(m.instrument().value()),
                   static_cast<int>(sbepp::to_underlying(m.kind())),
                   m.clientOrderIdField().value(),
                   m.price().value(),
                   m.qty().value());
    } else if (frame.schema_id == raikiri::schema::message_id<msgs::PnlIncrementMsg>()) {
        const auto m = raikiri::schema::view_msg<msgs::PnlIncrementMsg>(frame.payload);
        // Fixed SBE char fields are NUL- or space-padded; trimming only one of
        // the two leaves the other in the stream and the output stops being text.
        const std::string_view raw{m.symbol().data(), m.symbol().size()};
        const auto end = raw.find_last_not_of(std::string_view{"\0 ", 2});
        fmt::print("{}PNL   {} kind={} strat={} amount={} settle={}\n",
                   head,
                   end == std::string_view::npos ? std::string_view{} : raw.substr(0, end + 1),
                   static_cast<int>(sbepp::to_underlying(m.kind())),
                   m.strategyId().value(),
                   m.amount().value(),
                   static_cast<int>(sbepp::to_underlying(m.settleSide())));
    } else if (frame.schema_id == raikiri::schema::message_id<msgs::FundingMsg>()) {
        const auto m = raikiri::schema::view_msg<msgs::FundingMsg>(frame.payload);
        fmt::print("{}FUND  {} rate={} pred={} next={}\n",
                   head,
                   instrument_str(m.instrument().value()),
                   m.rate().value(),
                   m.predictedRate().value(),
                   m.nextFundingTime().value());
    } else if (frame.schema_id == raikiri::schema::message_id<msgs::ConnectivityMsg>()) {
        const auto m = raikiri::schema::view_msg<msgs::ConnectivityMsg>(frame.payload);
        fmt::print("{}CONN  venue={} event={} stream={}\n",
                   head,
                   static_cast<int>(sbepp::to_underlying(m.venue())),
                   static_cast<int>(sbepp::to_underlying(m.event())),
                   m.streamId().value());
    } else if (frame.schema_id == raikiri::schema::message_id<msgs::LatencySampleMsg>()) {
        const auto m = raikiri::schema::view_msg<msgs::LatencySampleMsg>(frame.payload);
        fmt::print("{}LAT   venue={} metric={} p50={} p99={} p999={} n={}\n",
                   head,
                   static_cast<int>(sbepp::to_underlying(m.venue())),
                   m.metric().value(),
                   m.p50Ns().value(),
                   m.p99Ns().value(),
                   m.p999Ns().value(),
                   m.count().value());
    } else {
        fmt::print(
            "{}?     schema_id={} ({} bytes)\n", head, frame.schema_id, frame.payload.size());
    }
}

} // namespace

int main(int argc, char** argv) {
    // The decoded stream stays on stdout, where golden-replay tests compare it
    // byte for byte; the logger writes to stderr and cannot reach it.
    const raikiri::log::Scope logging{"replay"};
    if (argc < 2) {
        fmt::print(stderr, "usage: replay <dir> [--prefix <name>] [--stats]\n");
        return 2;
    }
    std::string dir = argv[1];
    std::string prefix = "journal";
    bool stats_only = false;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--prefix") == 0 && i + 1 < argc) {
            prefix = argv[++i];
        } else if (std::strcmp(argv[i], "--stats") == 0) {
            stats_only = true;
        }
    }

    auto reader = raikiri::journal::JournalReader::open(dir, prefix);
    if (!reader.has_value()) {
        RLOG_ERROR("replay: cannot open journal in {} (prefix {})", dir, prefix);
        return 1;
    }

    std::map<std::uint16_t, std::uint64_t> counts;
    std::uint64_t total = 0;
    FrameView frame{};
    for (;;) {
        const auto result = reader->next(frame);
        if (result == raikiri::journal::ReadResult::Empty) {
            break;
        }
        if (result == raikiri::journal::ReadResult::Corrupt) {
            RLOG_ERROR("replay: CORRUPT frame after seq {}", reader->last_seq());
            return 3;
        }
        ++total;
        ++counts[frame.schema_id];
        if (!stats_only) {
            print_frame(frame);
        }
    }
    fmt::print("-- {} records", total);
    for (const auto& [id, n] : counts) {
        fmt::print("  [schema {}]={}", id, n);
    }
    fmt::print("\n");
    return 0;
}
