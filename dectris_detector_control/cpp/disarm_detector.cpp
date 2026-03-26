/*
 * Immediate disarm: send Simplon disarm without waiting for idle.
 *
 * Use to stop acquisition when you cannot or do not want to wait for the
 * detector to finish exposing (e.g. abort, emergency stop). For a clean
 * shutdown after a normal run, prefer wait_idle_and_disarm_detector.
 *
 * Same DCU host as the other producer tools.
 */

#include "eiger_session.hpp"
#include <cstdio>
#include <cstring>

int main(int argc, char *argv[]) {
    const char *const kDefaultHost = "169.254.254.1";
    const char *host = kDefaultHost;

    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            std::printf(
                    "Usage: %s [HOST]\n"
                    "  Send disarm immediately (does not wait for idle).\n"
                    "  Stops an armed acquisition sequence; stream frames may truncate.\n"
                    "  For normal end-of-run teardown after triggers, use:\n"
                    "    wait_idle_and_disarm_detector [HOST]\n",
                    argv[0]);
            return 0;
        }
        if (argv[i][0] == '-') {
            std::fprintf(stderr, "Unknown option: %s (try --help)\n", argv[i]);
            return 1;
        }
        host = argv[i];
    }

    const int kPort = 80;

    eiger_set_http_trace(stdout);
    EigerSession dcu(host, kPort);

    std::printf("Sending disarm...\n");
    if (dcu.sendCommand("disarm") != 0) {
        std::fprintf(stderr, "disarm failed\n");
        return 1;
    }
    std::printf("Disarm sent.\n");
    return 0;
}
