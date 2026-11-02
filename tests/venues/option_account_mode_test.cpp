#include <raikiri/exec/execution_client.hpp>
#include <raikiri/exec/option_account_mode.hpp>
#include <raikiri/venues/bybit/account.hpp>
#include <raikiri/venues/deribit/account.hpp>
#include <raikiri/venues/gate/account.hpp>
#include <raikiri/venues/okx/account.hpp>

#include <gtest/gtest.h>

#include <simdjson.h>

#include <string>

namespace {

using namespace std::string_view_literals;

std::string padded(std::string_view body) {
    std::string out{body};
    out.append(simdjson::SIMDJSON_PADDING, ' ');
    out.resize(body.size());
    return out;
}

TEST(OptionAccountMode, OkxReadsAcctLv) {
    const auto body = padded(R"({"code":"0","msg":"","data":[{"acctLv":"2","posMode":"net_mode"}]})");
    const auto mode = raikiri::venues::okx::parse_account_mode(body);
    ASSERT_TRUE(mode.has_value());
    EXPECT_EQ(raikiri::venues::okx::option_mode_name(*mode), "futures");
    const auto spot = raikiri::venues::okx::parse_account_mode(padded(R"({"code":"0","data":[{"acctLv":"1"}]})"));
    EXPECT_EQ(raikiri::venues::okx::option_mode_name(*spot), "spot");
    const auto odd = raikiri::venues::okx::parse_account_mode(padded(R"({"code":"0","data":[{"acctLv":"9"}]})"));
    EXPECT_EQ(raikiri::venues::okx::option_mode_name(*odd), "unknown");
}

TEST(OptionAccountMode, BybitReadsMarginMode) {
    const auto body = padded(R"({"unifiedMarginStatus":4,"marginMode":"REGULAR_MARGIN","dcpStatus":"OFF"})");
    EXPECT_EQ(raikiri::venues::bybit::parse_margin_mode(body).value(), "REGULAR_MARGIN");
    EXPECT_FALSE(raikiri::venues::bybit::parse_margin_mode(padded(R"({"unifiedMarginStatus":4})")).has_value());
}

TEST(OptionAccountMode, DeribitReadsThePortfolioFlag) {
    const auto standard = padded(R"({"result":{"currency":"BTC","portfolio_margining_enabled":false}})");
    EXPECT_EQ(raikiri::venues::deribit::parse_margin_regime(standard).value(), "standard");
    const auto portfolio = padded(R"({"result":{"portfolio_margining_enabled":true,"currency":"BTC"}})");
    EXPECT_EQ(raikiri::venues::deribit::parse_margin_regime(portfolio).value(), "portfolio");
    EXPECT_FALSE(raikiri::venues::deribit::parse_margin_regime(padded(R"({"result":{"currency":"BTC"}})")).has_value());
}

TEST(OptionAccountMode, GateReadsMarginModeAsItsNumber) {
    const auto body = padded(R"({"user":1,"total":"10","available":"8","currency":"USDT","margin_mode":1})");
    EXPECT_EQ(raikiri::venues::gate::parse_option_margin_mode(body).value(), 1U);
    EXPECT_FALSE(raikiri::venues::gate::parse_option_margin_mode(padded(R"({"user":1})")).has_value());
}

TEST(OptionModeCheck, AMismatchOrAnUnreadModeStopsOptions) {
    using raikiri::exec::check_option_mode;
    using raikiri::exec::VenueError;
    using Reading = std::expected<std::string, VenueError>;
    EXPECT_TRUE(check_option_mode("futures", Reading{"futures"}).may_trade);
    EXPECT_TRUE(check_option_mode("", Reading{std::unexpected{raikiri::exec::not_implemented("x")}}).may_trade);
    const auto differs = check_option_mode("futures", Reading{"portfolio"});
    EXPECT_FALSE(differs.may_trade);
    EXPECT_NE(differs.why.find("portfolio"), std::string::npos);
    const auto unread = check_option_mode("futures", Reading{std::unexpected{raikiri::exec::not_implemented("x")}});
    EXPECT_FALSE(unread.may_trade);
    EXPECT_NE(unread.why.find("did not say"), std::string::npos);
}

TEST(OptionAccountMode, ClosingStaysAllowedUnderAMismatch) {
    using raikiri::exec::closes_position;
    const auto lots = [](std::int64_t v) { return raikiri::QtyLots::from_raw(v); };
    EXPECT_TRUE(closes_position(lots(-5), raikiri::Side::Buy, lots(5)));
    EXPECT_TRUE(closes_position(lots(5), raikiri::Side::Sell, lots(2)));
    EXPECT_FALSE(closes_position(lots(-5), raikiri::Side::Buy, lots(6))) << "past flat is new risk";
    EXPECT_FALSE(closes_position(lots(-5), raikiri::Side::Sell, lots(1)));
    EXPECT_FALSE(closes_position(lots(0), raikiri::Side::Buy, lots(1)));
    EXPECT_FALSE(closes_position(raikiri::QtyLots::undef(), raikiri::Side::Buy, lots(1)));
}

} // namespace
