#include <raikiri/clientapi/presentation/frames.hpp>

#include <charconv>

#include "json.hpp"

namespace raikiri::clientapi {

namespace wire {

struct Problem {
    std::string type{};
    std::string title{};
    std::int32_t status{0};
    std::optional<std::string> detail{};
    std::string code{};
    std::optional<std::string> request_id{};
};

struct ProblemFrame {
    std::string type{"problem"};
    std::optional<std::string> id{};
    Problem problem{};
};

template <typename T> struct SeqOrder {
    std::string type{"order"};
    std::string seq{};
    T order_event{};
};

template <typename T> struct SeqFill {
    std::string type{"fill"};
    std::string seq{};
    T fill{};
};

template <typename T> struct SeqPosition {
    std::string type{"position"};
    std::string seq{};
    T position{};
};

template <typename T> struct SeqBalance {
    std::string type{"balance"};
    std::string seq{};
    T balance{};
};

template <typename T> struct SeqAlgo {
    std::string type{"algo"};
    std::string seq{};
    T algo{};
};

struct SeqAlgoLost {
    std::string type{"algo_lost"};
    std::string seq{};
    AlgoLost algo_lost{};
};

struct SeqGap {
    std::string type{"gap"};
    std::string seq{};
    std::string reason{};
};

struct Hello {
    std::string type{"hello"};
    std::string epoch{};
    std::string seq{};
    std::uint32_t heartbeat_ms{0};
    std::string client_id{};
};

struct Heartbeat {
    std::string type{"heartbeat"};
    std::string ts{};
    std::string seq{};
};

struct Ack {
    std::string type{"ack"};
    std::string id{};
    std::string seq{};
};

struct Pong {
    std::string type{"pong"};
    std::string id{};
};

struct Gap {
    std::string type{"gap"};
    std::string from{};
    std::string to{};
    std::string reason{};
};

struct ClientMessage {
    std::string op{};
    std::string id{};
    std::optional<std::vector<std::string>> channels{};
    std::optional<std::string> resume_from{};
    std::optional<std::string> epoch{};
    std::optional<std::vector<std::string>> quotes{};
    std::optional<std::string> book{};
    std::optional<std::int64_t> levels{};
    std::optional<std::vector<std::string>> greeks{};
    std::optional<std::vector<std::string>> funding{};
    std::optional<std::vector<std::string>> ticker{};
    std::optional<glz::raw_json> hz{};
};

struct MarketSubscribe {
    std::string op{"subscribe"};
    std::string id{};
    std::optional<std::vector<std::string>> quotes{};
    std::optional<std::string> book{};
    std::optional<std::int64_t> levels{};
    std::optional<std::vector<std::string>> greeks{};
    std::optional<std::vector<std::string>> funding{};
    std::optional<std::vector<std::string>> ticker{};
    std::optional<glz::raw_json> hz{};
};

struct MarketUnsubscribe {
    std::string op{"unsubscribe"};
    std::string id{};
};

struct LinkSubscribe {
    std::string op{"subscribe"};
    std::string id{"private"};
    std::vector<std::string> events{"orders", "fills"};
};

struct UpstreamWire {
    std::string type{};
    std::optional<OrderEventRow> order_event{};
    std::optional<FillRow> fill{};
    std::optional<std::string> missed{};
};

} // namespace wire

std::string problem_body(const Refusal& refusal, std::string_view request_id) {
    wire::Problem body{.type = "urn:raikiri:problem:" + refusal.code,
                       .title = refusal.title,
                       .status = refusal.status,
                       .code = refusal.code,
                       .request_id = std::string{request_id}};
    if (!refusal.detail.empty()) {
        body.detail = refusal.detail;
    }
    return internal::to_json(body);
}

std::string private_frame(const PrivateEvent& event) {
    const auto seq = std::to_string(event.seq);
    return std::visit(
        [&seq](const auto& payload) -> std::string {
            using T = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<T, OrderEventRow>) {
                return internal::to_json(wire::SeqOrder<T>{.seq = seq, .order_event = payload});
            } else if constexpr (std::is_same_v<T, FillRow>) {
                return internal::to_json(wire::SeqFill<T>{.seq = seq, .fill = payload});
            } else if constexpr (std::is_same_v<T, PositionRow>) {
                return internal::to_json(wire::SeqPosition<T>{.seq = seq, .position = payload});
            } else if constexpr (std::is_same_v<T, AccountBalances>) {
                return internal::to_json(wire::SeqBalance<T>{.seq = seq, .balance = payload});
            } else if constexpr (std::is_same_v<T, ParentRow>) {
                return internal::to_json(wire::SeqAlgo<T>{.seq = seq, .algo = payload});
            } else if constexpr (std::is_same_v<T, AlgoLost>) {
                return internal::to_json(wire::SeqAlgoLost{.seq = seq, .algo_lost = payload});
            } else {
                return internal::to_json(wire::SeqGap{.seq = seq, .reason = payload.reason});
            }
        },
        event.payload);
}

std::string
hello_frame(std::string_view epoch, std::uint64_t seq, std::uint32_t heartbeat_ms, std::string_view client_id) {
    return internal::to_json(wire::Hello{.epoch = std::string{epoch},
                                         .seq = std::to_string(seq),
                                         .heartbeat_ms = heartbeat_ms,
                                         .client_id = std::string{client_id}});
}

std::string heartbeat_frame(UnixNanos ts, std::uint64_t seq) {
    return internal::to_json(wire::Heartbeat{.ts = std::to_string(ts.ns), .seq = std::to_string(seq)});
}

std::string ack_frame(std::string_view id, std::uint64_t seq) {
    return internal::to_json(wire::Ack{.id = std::string{id}, .seq = std::to_string(seq)});
}

std::string pong_frame(std::string_view id) {
    return internal::to_json(wire::Pong{.id = std::string{id}});
}

std::string gap_frame(std::uint64_t from, std::uint64_t to, std::string_view reason) {
    return internal::to_json(
        wire::Gap{.from = std::to_string(from), .to = std::to_string(to), .reason = std::string{reason}});
}

std::string stream_problem_frame(std::string_view id, const Refusal& refusal) {
    wire::ProblemFrame frame{};
    if (!id.empty()) {
        frame.id = std::string{id};
    }
    frame.problem = {.type = "urn:raikiri:problem:" + refusal.code,
                     .title = refusal.title,
                     .status = refusal.status,
                     .code = refusal.code};
    if (!refusal.detail.empty()) {
        frame.problem.detail = refusal.detail;
    }
    return internal::to_json(frame);
}

std::expected<ClientOp, Refusal> parse_client_op(std::string_view text) {
    wire::ClientMessage message{};
    if (!internal::from_body(message, text)) {
        return std::unexpected(
            Refusal{.status = 400, .code = "bad-request", .title = "not a message this stream takes"});
    }
    ClientOp op{.op = message.op, .id = message.id};
    if (op.op != "subscribe" && op.op != "unsubscribe" && op.op != "ping") {
        return std::unexpected(Refusal{.status = 400, .code = "bad-request", .title = "unknown op", .detail = op.op});
    }
    if (op.op != "subscribe") {
        return op;
    }
    for (const auto& name : message.channels.value_or(std::vector<std::string>{})) {
        if (name == "orders") {
            op.channels |= mask_of(Channel::Orders);
        } else if (name == "fills") {
            op.channels |= mask_of(Channel::Fills);
        } else if (name == "positions") {
            op.channels |= mask_of(Channel::Positions);
        } else if (name == "balances") {
            op.channels |= mask_of(Channel::Balances);
        } else if (name == "algos") {
            op.channels |= mask_of(Channel::Algos);
        } else {
            return std::unexpected(
                Refusal{.status = 400, .code = "bad-request", .title = "unknown channel", .detail = name});
        }
    }
    if (message.resume_from.has_value()) {
        const auto& text_seq = *message.resume_from;
        std::uint64_t seq = 0;
        const auto [stop, ec] = std::from_chars(text_seq.data(), text_seq.data() + text_seq.size(), seq);
        if (ec != std::errc{} || stop != text_seq.data() + text_seq.size()) {
            return std::unexpected(Refusal{.status = 400, .code = "bad-request", .title = "resume_from is digits"});
        }
        op.resume_from = seq;
    }
    op.epoch = message.epoch.value_or(std::string{});
    const auto collect = [&op](const std::optional<std::vector<std::string>>& ids) {
        for (const auto& id : ids.value_or(std::vector<std::string>{})) {
            op.instruments.push_back(id);
        }
    };
    collect(message.quotes);
    collect(message.greeks);
    collect(message.funding);
    collect(message.ticker);
    if (message.book.has_value()) {
        op.instruments.push_back(*message.book);
    }
    if (!op.instruments.empty()) {
        op.market_message = internal::to_json(wire::MarketSubscribe{.id = message.id,
                                                                    .quotes = message.quotes,
                                                                    .book = message.book,
                                                                    .levels = message.levels,
                                                                    .greeks = message.greeks,
                                                                    .funding = message.funding,
                                                                    .ticker = message.ticker,
                                                                    .hz = message.hz});
    } else if (op.channels == 0) {
        return std::unexpected(Refusal{.status = 400, .code = "bad-request", .title = "subscribe to something"});
    }
    return op;
}

std::string market_unsubscribe(std::string_view id) {
    return internal::to_json(wire::MarketUnsubscribe{.id = std::string{id}});
}

std::string private_link_subscribe() {
    return internal::to_json(wire::LinkSubscribe{});
}

std::optional<UpstreamFrame> parse_upstream_frame(std::string_view text) {
    wire::UpstreamWire frame{};
    if (!internal::from_backend(frame, text)) {
        return std::nullopt;
    }
    if (frame.type == "gap") {
        return UpstreamFrame{.gap = "the backend's event ring overran; missed " + frame.missed.value_or("?")};
    }
    if (frame.type != "event") {
        return std::nullopt;
    }
    return UpstreamFrame{.order_event = std::move(frame.order_event), .fill = std::move(frame.fill)};
}

} // namespace raikiri::clientapi
