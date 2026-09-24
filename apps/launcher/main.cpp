// launcher: dev/demo supervisor for gatewayd(sim) and strategyd behind `make demo`

#include <raikiri/log/log.hpp>

#include <fmt/format.h>

#include <libgen.h>
#include <sys/wait.h>
#include <unistd.h>

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

pid_t spawn(const std::string& path, const std::vector<std::string>& args) {
    const pid_t pid = ::fork();
    if (pid != 0) {
        return pid;
    }
    std::vector<char*> argv;
    argv.push_back(::strdup(path.c_str()));
    for (const auto& arg : args) {
        argv.push_back(::strdup(arg.c_str()));
    }
    argv.push_back(nullptr);
    ::execv(path.c_str(), argv.data());
    // no logger between fork and exec
    fmt::print(stderr, "launcher: exec {} failed\n", path);
    ::_exit(127);
}

} // namespace

int main(int argc, char** argv) {
    std::string duration = "10";
    for (int i = 1; i < argc; ++i) {
        if (std::string_view{argv[i]} == "--duration" && i + 1 < argc) {
            duration = argv[++i];
        }
    }

    char self[4096];
    std::snprintf(self, sizeof self, "%s", argv[0]);
    const std::string bin_dir = ::dirname(self);
    const auto gatewayd = bin_dir + "/../gatewayd/gatewayd";
    const auto strategyd = bin_dir + "/../strategyd/strategyd";

    const pid_t gw = spawn(gatewayd, {"--venue=sim"});
    const pid_t st = spawn(strategyd, {"--duration", duration});

    // start logging after the forks; spawn() allocates
    const raikiri::log::Scope logging{"launcher"};

    int status = 0;
    ::waitpid(st, &status, 0);
    const int strategy_rc = WIFEXITED(status) ? WEXITSTATUS(status) : 128;

    ::kill(gw, SIGTERM);
    ::waitpid(gw, &status, 0);
    const int gateway_rc = WIFEXITED(status) ? WEXITSTATUS(status) : 128;

    if (strategy_rc != 0 || gateway_rc != 0) {
        RLOG_ERROR("launcher: strategyd rc={} gatewayd rc={}", strategy_rc, gateway_rc);
        return 1;
    }
    return 0;
}
