// omatrack-cli — test-only entry point for the headless commands that ship
// inside the `omatrack` binary (`omatrack parse …`). Built for CTest and the
// benchmark scripts; never installed or packaged.

#include "Headless.h"

#include <cstdio>
#include <cstring>

int main(int argc, char** argv) {
    if (argc == 2 && (std::strcmp(argv[1], "--version") == 0 ||
                      std::strcmp(argv[1], "-V") == 0)) {
        std::printf("omatrack-cli %s\n", OMATRACK_VERSION);
        return 0;
    }
    if (argc < 2 || !omatrack::headless::isCommand(argv[1])) {
        omatrack::headless::printUsage(argv[0]);
        return 2;
    }
    return omatrack::headless::run(argc, argv, argv[0]);
}
