// launcher — dev/demo supervisor: spawn gatewayd(sim) + strategyd, wait for
// the strategy run to finish, take the gateway down, report. Production uses
// systemd units; this exists so `make demo` is one command.

#include <fmt/format.h>

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <libgen.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
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

    // Sibling binaries: <build>/apps/launcher/launcher -> ../gatewayd/gatewayd.
    char self[4096];
    std::snprintf(self, sizeof self, "%s", argv[0]);
    const std::string bin_dir = ::dirname(self);
    const auto gatewayd = bin_dir + "/../gatewayd/gatewayd";
    const auto strategyd = bin_dir + "/../strategyd/strategyd";

    const pid_t gw = spawn(gatewayd, {"--venue=sim"});
    const pid_t st = spawn(strategyd, {"--duration", duration});

    int status = 0;
    ::waitpid(st, &status, 0);
    const int strategy_rc = WIFEXITED(status) ? WEXITSTATUS(status) : 128;

    ::kill(gw, SIGTERM);
    ::waitpid(gw, &status, 0);
    const int gateway_rc = WIFEXITED(status) ? WEXITSTATUS(status) : 128;

    if (strategy_rc != 0 || gateway_rc != 0) {
        fmt::print(stderr, "launcher: strategyd rc={} gatewayd rc={}\n", strategy_rc,
                   gateway_rc);
        return 1;
    }
    return 0;
}
