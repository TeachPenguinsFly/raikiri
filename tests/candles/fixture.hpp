#pragma once
// a captured venue answer from tests/candles/fixtures

#include <fstream>
#include <sstream>
#include <string>

#ifndef RAIKIRI_CANDLE_FIXTURES
#error "RAIKIRI_CANDLE_FIXTURES names the fixture directory"
#endif

inline std::string candle_fixture(const std::string& name) {
    std::ifstream file{std::string{RAIKIRI_CANDLE_FIXTURES} + "/" + name};
    std::stringstream buf;
    buf << file.rdbuf();
    return buf.str();
}
