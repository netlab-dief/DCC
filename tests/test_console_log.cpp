#include "console_log.h"
#include <cstdlib>
#include <iomanip>
#include <set>
#include <thread>
#include <vector>

namespace {
void require(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}

void test_console_lines() {
    std::ostringstream output;
    std::vector<std::thread> workers;
    for (int thread = 0; thread < 4; ++thread) workers.emplace_back([&, thread] {
        for (int line = 0; line < 100; ++line) ConsoleLine(output) << thread << ' ' << line << std::endl;
    });
    for (auto& worker : workers) worker.join();
    std::istringstream input(output.str());
    std::set<std::pair<int, int>> received;
    std::string line;
    int count = 0;
    while (std::getline(input, line)) {
        int thread = -1, index = -1;
        std::string extra;
        std::istringstream fields(line);
        require(static_cast<bool>(fields >> thread >> index) && !(fields >> extra) &&
            thread >= 0 && thread < 4 && index >= 0 && index < 100, "concurrent output preserves complete lines");
        received.emplace(thread, index);
        ++count;
    }
    require(count == 400 && received.size() == 400, "no lost or merged monitor lines");
    std::ostringstream formatting;
    ConsoleLine(formatting) << std::fixed << std::setprecision(2) << 1.2345;
    ConsoleLine(formatting) << 1.2345;
    require(formatting.str() == "1.23\n1.2345\n", "formatting remains local to each message");
}
} // namespace
int main() {
    test_console_lines();
    std::cout << "Concurrent logging tests passed\n";
}
