#include <raikiri/venues/blofin/ws.hpp>

#include <cstring>

namespace raikiri::venues::blofin {

std::expected<std::string_view, WsBuildError>
Protocol::op_frame(std::string_view op, std::string_view inst_id, std::span<const std::string_view> channels) {
    if (inst_id.empty() || channels.empty()) {
        return std::unexpected{WsBuildError::NothingToSend};
    }
    std::size_t len = 0;
    bool overflow = false;
    const auto put = [&](std::string_view text) {
        if (overflow || len + text.size() > frame_.size()) {
            overflow = true;
            return;
        }
        text.copy(frame_.data() + len, text.size());
        len += text.size();
    };
    put(R"({"op":")");
    put(op);
    put(R"(","args":[)");
    for (std::size_t i = 0; i < channels.size(); ++i) {
        put(i == 0 ? R"({"channel":")" : R"(,{"channel":")");
        put(channels[i]);
        put(R"(","instId":")");
        put(inst_id);
        put(R"("})");
    }
    put("]}");
    if (overflow) {
        return std::unexpected{WsBuildError::BufferTooSmall};
    }
    return std::string_view{frame_.data(), len};
}

std::expected<std::string_view, WsBuildError> Protocol::subscribe_frame(std::string_view inst_id,
                                                                        std::span<const std::string_view> channels) {
    return op_frame("subscribe", inst_id, channels);
}

std::expected<std::string_view, WsBuildError> Protocol::unsubscribe_frame(std::string_view inst_id,
                                                                          std::span<const std::string_view> channels) {
    return op_frame("unsubscribe", inst_id, channels);
}

} // namespace raikiri::venues::blofin
