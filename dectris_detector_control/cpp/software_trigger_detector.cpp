/*
 * EIGER demo (part 2): send software trigger(s) to an already-armed detector only.
 *
 * Run after connect_and_configure_and_arm_detector (same DCU host). -n must match ntrigger.
 * After triggers complete, run wait_idle_and_disarm_detector (same HOST) to wait for idle and disarm.
 */

#include "eiger_session.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>

int main(int argc, char *argv[]) {
    // =============================================================================
    // USER INPUT
    // =============================================================================
    const char *const kDefaultHost = "169.254.254.1";
    const char *host = kDefaultHost;
    int n_triggers = 1;

    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            std::printf("Usage: %s [-n N] [HOST]\n"
                        "  Send N software trigger commands (default 1). No wait-for-idle, no disarm.\n"
                        "  Detector must already be armed (connect_and_configure_and_arm_detector).\n"
                        "  Then run: wait_idle_and_disarm_detector [HOST]\n"
                        "  -n N   number of triggers (must match ntrigger from part 1)\n",
                        argv[0]);
            return 0;
        }
        if (std::strcmp(argv[i], "-n") == 0) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "-n requires a number\n");
                return 1;
            }
            n_triggers = std::atoi(argv[++i]);
            if (n_triggers < 1) {
                std::fprintf(stderr, "-n must be >= 1\n");
                return 1;
            }
            continue;
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

    // =============================================================================
    // TRIGGER (detector must be armed)
    // =============================================================================
    std::printf("Sending %d software trigger(s)...\n", n_triggers);
    for (int i = 0; i < n_triggers; i++) {
        std::printf("Triggering image %d/%d...\n", i + 1, n_triggers);
        if (dcu.sendCommand("trigger") != 0) {
            std::fprintf(stderr, "trigger failed (try wait_idle_and_disarm_detector to recover)\n");
            dcu.sendCommand("disarm");
            return 1;
        }
    }

    std::printf("Triggers sent. Run: wait_idle_and_disarm_detector");
    if (host != kDefaultHost)
        std::printf(" %s", host);
    std::printf("\n");

    return 0;
}
