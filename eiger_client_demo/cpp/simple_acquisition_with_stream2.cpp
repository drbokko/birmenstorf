/*
 * Simple EIGER acquisition with stream2-style settings (monitor/filewriter off, stream on).
 *
 * Mirror of: stream_v2/examples/eiger_client_demo/python/simple_acquisition_with_stream2.py
 */

#include "eiger_session.hpp"
#include <cstdio>
#include <cstring>

namespace {

bool stateIsIdle(const char *status_json) {
    return status_json && std::strstr(status_json, "\"idle\"") != nullptr;
}

int jsonValueNumber(const char *json, double *out) {
    const char *p = std::strstr(json, "\"value\"");
    if (!p)
        return -1;
    p = std::strchr(p, ':');
    if (!p)
        return -1;
    ++p;
    while (*p == ' ' || *p == '\t')
        ++p;
    return std::sscanf(p, "%lf", out) == 1 ? 0 : -1;
}

void printStatusLine(const char *label, const char *json) {
    if (json && json[0])
        std::printf("%s%s\n", label, json);
}

} // namespace

int main(int argc, char *argv[]) {
    // =============================================================================
    // USER INPUT
    // =============================================================================
    const char *const kDefaultHost = "169.254.254.1";
    const char *host = kDefaultHost;
    int force_initialization = 0; // Set via --force-init (always call initialize)
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--force-init") == 0) {
            force_initialization = 1;
        } else if (std::strcmp(argv[i], "--help") == 0) {
            std::printf("Usage: %s [--force-init] [HOST]\n"
                        "  HOST  DCU hostname or IP (default %s)\n"
                        "  --force-init  Call initialize even when state is idle\n",
                        argv[0], kDefaultHost);
            return 0;
        } else if (argv[i][0] == '-') {
            std::fprintf(stderr, "Unknown option: %s (try --help)\n", argv[i]);
            return 1;
        } else {
            host = argv[i];
        }
    }
    const int kPort = 80;
    // Data acquisition (single threshold here; Python may use a list of 1–2)
    const int threshold_ev = 15000;
    const int number_of_images = 1000;
    const int number_of_triggers = 1; // Each trigger acquires number_of_images frames
    const double exposure_time = 1.0 / 1000.0; // Count time per image [s] (e.g. 1/fps)
    const double sleep_time = 0.0; // Extra delay per frame [s]; frame_time = exposure + sleep

    // =============================================================================
    // CONNECT TO DETECTOR
    // =============================================================================
    EigerSession dcu(host, kPort);

    char response[EIGER_CLIENT_RESPONSE_MAX];
    char buf[64];

    // =============================================================================
    // INITIALIZE
    // =============================================================================
    if (dcu.getStatus("state", response, sizeof(response)) != 0) {
        std::fprintf(stderr, "Failed to read detector state (host %s:%d)\n", dcu.host(), dcu.port());
        return 1;
    }
    std::printf("Detector status:\t\t\t%s\n", response);

    // Initialize if forced or not idle
    if (force_initialization || !stateIsIdle(response)) {
        std::printf("Initializing detector...\n");
        if (dcu.sendCommand("initialize") != 0) {
            std::fprintf(stderr, "initialize failed\n");
            return 1;
        }
    }

    // =============================================================================
    // CONFIGURATION
    // =============================================================================
    // Usual settings for polychromatic beam
    dcu.setDetectorConfig("countrate_correction_applied", "false");
    dcu.setDetectorConfig("retrigger", "false");
    dcu.setDetectorConfig("counting_mode", "\"normal\"");
    dcu.setDetectorConfig("flatfield_correction_applied", "false");
    dcu.setDetectorConfig("virtual_pixel_correction_applied", "true");
    dcu.setDetectorConfig("mask_to_zero", "true");
    dcu.setDetectorConfig("auto_summation", "false");

    // Thresholds and timing
    dcu.setDetectorConfig("threshold/1/mode", "\"enabled\"");
    std::snprintf(buf, sizeof(buf), "%d", threshold_ev);
    dcu.setDetectorConfig("threshold/1/energy", buf);
    dcu.setDetectorConfig("threshold/2/mode", "\"disabled\"");

    std::snprintf(buf, sizeof(buf), "%.9f", exposure_time);
    dcu.setDetectorConfig("count_time", buf);
    std::snprintf(buf, sizeof(buf), "%.9f", exposure_time + sleep_time);
    dcu.setDetectorConfig("frame_time", buf);
    std::snprintf(buf, sizeof(buf), "%d", number_of_images);
    dcu.setDetectorConfig("nimages", buf);
    std::snprintf(buf, sizeof(buf), "%d", number_of_triggers);
    dcu.setDetectorConfig("ntrigger", buf);

    // Useful status readout
    if (dcu.getStatus("high_voltage/state", response, sizeof(response)) == 0)
        printStatusLine("High voltage status:\t\t", response);
    if (dcu.getStatus("temperature", response, sizeof(response)) == 0) {
        double t = 0.0;
        if (jsonValueNumber(response, &t) == 0)
            std::printf("Temperature:\t\t\t%.1f deg\n", t);
        else
            printStatusLine("Temperature:\t\t\t", response);
    }
    if (dcu.getStatus("humidity", response, sizeof(response)) == 0) {
        double h = 0.0;
        if (jsonValueNumber(response, &h) == 0)
            std::printf("Humidity:\t\t\t%.1f %%\n", h);
        else
            printStatusLine("Humidity:\t\t\t", response);
    }

    std::printf("Count time= %.9f s\n", exposure_time);
    std::printf("Frame time= %.9f s\n", exposure_time + sleep_time);

    // =============================================================================
    // DATA ACQUISITION INTERFACES
    // =============================================================================
    dcu.setMonitorConfig("mode", "disabled");
    dcu.setFilewriterConfig("mode", "\"disabled\"");
    dcu.setStreamConfig("mode", "\"enabled\"");
    dcu.setStreamConfig("format", "\"cbor\"");
    dcu.setStreamConfig("header_detail", "\"all\"");

    // =============================================================================
    // RUN ACQUISITION
    // =============================================================================
    std::printf("Acquiring data...\n");
    if (dcu.sendCommand("arm") != 0) {
        std::fprintf(stderr, "arm failed\n");
        return 1;
    }
    // Software triggers; comment out if using external trigger
    for (int i = 0; i < number_of_triggers; i++) {
        std::printf("Triggering image %d/%d...\n", i + 1, number_of_triggers);
        if (dcu.sendCommand("trigger") != 0) {
            std::fprintf(stderr, "trigger failed\n");
            dcu.sendCommand("disarm");
            return 1;
        }
    }
    if (dcu.sendCommand("disarm") != 0)
        std::fprintf(stderr, "Warning: disarm failed\n");

    return 0;
}
