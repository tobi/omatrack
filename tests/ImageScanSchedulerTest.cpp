#include "inference/ImageScanScheduler.h"
#include <array>
#include <iostream>
#include <stdexcept>

using omatrack::inference::ImageScanScheduler;
int main() {
    try {
        ImageScanScheduler plan;
        std::array<bool, 5> visited{};
        auto has = [&](std::size_t i) { return visited[i]; };
        auto next = [&](std::size_t cursor, bool ahead) {
            return plan.next(visited.size(), cursor, ahead, has);
        };
        auto require = [](bool ok) {
            if (!ok) throw std::runtime_error("scan schedule contract");
        };
        plan.fromCursor(3);
        require(next(3, true) == 3);
        visited[3] = true;
        require(next(3, true) == 4);
        visited[4] = true;
        require(next(3, true) == 0);
        visited[0] = true;
        require(next(3, true) == 1);
        visited[1] = true;
        require(next(3, true) == 2);
        visited[2] = true;
        require(!next(3, true));  // complete does no duplicate work
        visited[2] = false;
        require(!next(3, false));  // watch mode never fills unrelated holes
        require(next(2, false) == 2);
        plan.fromCursor(4);
        require(next(2, true) == 2);  // live cursor outranks ahead pointer
        require(!plan.next(0, 100, true, has));
        std::cout << "PASS cursor priority, forward scan, wrap/backfill, "
                     "watch-only, completion\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
