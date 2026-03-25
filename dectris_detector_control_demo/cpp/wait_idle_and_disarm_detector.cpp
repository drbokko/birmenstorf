/*
 * EIGER demo (part 3): poll detector state until idle, then disarm.
 *
 * Run after software_trigger_detector when acquisition should be finished.
 * Same DCU host as parts 1 and 2.
 */

#include "eiger_session.hpp"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

namespace {

bool stateIsIdle(const char *status_json) {
    return status_json && std::strstr(status_json, "\"idle\"") != nullptr;
}

} // namespace

int main(int argc, char *argv[]) {
    // =============================================================================
    // USER INPUT
    // =============================================================================
    const char *const kDefaultHost = "169.254.254.1";
    const char *host = kDefaultHost;

    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            std::printf("Usage: %s [HOST]\n"
                        "  Poll GET state until value is idle, then PUT disarm.\n"
                        "  Run after software_trigger_detector when the run is complete.\n",
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

    // =============================================================================
    // CONNECT TO DETECTOR
    // =============================================================================
    eiger_set_http_trace(stdout); // [SIMPLON API] -> request trace: method, URL, PUT body
    EigerSession dcu(host, kPort);
    char response[EIGER_CLIENT_RESPONSE_MAX];

    // =============================================================================
    // Wait for idle, then disarm
    // =============================================================================
    std::printf("Waiting for idle...\n");
    for (;;) {
        if (dcu.getStatus("state", response, sizeof(response)) != 0) {
            std::fprintf(stderr, "Failed to read state while waiting for idle\n");
            dcu.sendCommand("disarm");
            return 1;
        }
        std::printf("[SIMPLON API] <- %s", response);
        if (stateIsIdle(response))
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::printf("Disarming...\n");
    if (dcu.sendCommand("disarm") != 0)
        std::fprintf(stderr, "Warning: disarm failed\n");

    return 0;
}
