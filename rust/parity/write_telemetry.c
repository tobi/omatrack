/* Parity helper: write a native .telemetry companion of a source recording
 * through the Qt bridge (the converter the C++ app used), so the Rust port's
 * .telemetry reader can be diffed against the oracle on real data.
 * Usage: write-telemetry <source> <out.telemetry>. Never writes beside the
 * source; the caller chooses an output under rust/parity/out/. */
#include <stdio.h>
#include "omatrack_bridge.h"

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <source> <out.telemetry>\n", argv[0]);
        return 2;
    }
    void *handle = omatrack_open(argv[1]);
    if (!handle) {
        fprintf(stderr, "open: %s\n", omatrack_last_error());
        return 1;
    }
    int ok = omatrack_write_telemetry(handle, argv[2]);
    if (!ok) fprintf(stderr, "write: %s\n", omatrack_last_error());
    omatrack_close(handle);
    return ok ? 0 : 1;
}
