/*
 * EIGER demo (part 1): configure the DCU and arm for acquisition. Does not send software trigger.
 *
 * Then `software_trigger_detector` (same HOST, matching -n), then `wait_idle_and_disarm_detector`.
 *
 * Workflow: connect; initialize; CONFIGURATION (IMPORTANT); housekeeping; stream/monitor/filewriter; arm.
 * Parts 2–3: software_trigger_detector, then wait_idle_and_disarm_detector. Stream consumer is separate.
 *
 * Disclaimer: ntrigger / nimages must match software_trigger_detector -n and your measurement.
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
                        "  Configure detector, stream (CBOR), then arm. No trigger.\n"
                        "  Then: software_trigger_detector [-n N] [HOST], then wait_idle_and_disarm_detector [HOST].\n",
                        argv[0]);
            return 0;
        } else if (argv[i][0] == '-') {
            std::fprintf(stderr, "Unknown option: %s (try --help)\n", argv[i]);
            return 1;
        } else {
            host = argv[i];
        }
    }
    const int kPort = 80;
    const int threshold_ev = 15000;
    const int number_of_images = 1000;
    const int number_of_triggers = 100000; // Acquisition sequence can be stopped any time with the "disarm" command
    const double exposure_time = 1.0 / 1000.0; // Count time per image [s] (e.g. 1/fps)
    const double sleep_time = 0.0; // Extra delay per frame [s]; frame_time = exposure + sleep

    // =============================================================================
    // CONNECT TO DETECTOR
    // =============================================================================
    eiger_set_http_trace(stdout); // [SIMPLON API] -> request trace: method, URL, PUT body
    EigerSession dcu(host, kPort);

    char response[EIGER_CLIENT_RESPONSE_MAX];

    // =============================================================================
    // INITIALIZE
    // =============================================================================
    if (dcu.getStatus("state", response, sizeof(response)) != 0) {
        std::fprintf(stderr, "Failed to read detector state (host %s:%d)\n", dcu.host(), dcu.port());
        return 1;
    }
    std::printf("[SIMPLON API] <- %s", response);

    // Initialize if forced or not idle
    if (force_initialization || !stateIsIdle(response)) {
        std::printf("Initializing detector...\n");
        if (dcu.sendCommand("initialize") != 0) {
            std::fprintf(stderr, "initialize failed\n");
            return 1;
        }
    }

    // =============================================================================
    // CONFIGURATION  (IMPORTANT)
    // =============================================================================
    // Detector (Simplon-style layout for this stream demo):
    //   - Disable: countrate_correction_applied, retrigger, flatfield_correction_applied,
    //              auto_summation.
    //
    //   - Enable: virtual_pixel_correction_applied, mask_to_zero.
    //   - Set: threshold mode/energy, count_time, frame_time, nimages, ntrigger.
    //   - Note:
    //     - counting_mode is a string parameter (not a bool): set explicitly as required;
    //     - Do NOT set photon_energy: it can overwrite threshold-related settings.
    //     - trigger_mode is a string parameter, "ints" for internal trigger, "exts" for external trigger.
    //       Default value is ints but safer to set it explicitly. 1 trigger will acquire nimages frames.

    // Data path:
    //   Disable monitor; disable filewriter; enable stream (CBOR etc. below).
    // Usual settings for polychromatic beam
    dcu.setDetectorConfig("countrate_correction_applied", false);
    dcu.setDetectorConfig("retrigger", false);
    dcu.setDetectorConfig("counting_mode", "normal"); // see IMPORTANT: mode string, not "disabled"
    dcu.setDetectorConfig("flatfield_correction_applied", false);
    dcu.setDetectorConfig("virtual_pixel_correction_applied", true);
    dcu.setDetectorConfig("mask_to_zero", true);
    dcu.setDetectorConfig("auto_summation", false);

    // Thresholds and timing (EigerSession builds JSON value fragments)
    dcu.setDetectorConfig("threshold/1/mode", "enabled");
    dcu.setDetectorConfig("threshold/1/energy", threshold_ev);
    dcu.setDetectorConfig("threshold/2/mode", "disabled");

    dcu.setDetectorConfig("count_time", exposure_time);
    dcu.setDetectorConfig("frame_time", exposure_time + sleep_time);
    dcu.setDetectorConfig("nimages", number_of_images);
    dcu.setDetectorConfig("trigger_mode", "ints");
    dcu.setDetectorConfig("ntrigger", number_of_triggers);

    std::printf("[SIMPLON API] -> count_time= %.9f s (configured)\n", exposure_time);
    std::printf("[SIMPLON API] -> frame_time= %.9f s (configured)\n", exposure_time + sleep_time);

    // =============================================================================
    // After configuration check : high voltage, temperature, humidity
    // =============================================================================
    if (dcu.getStatus("high_voltage/state", response, sizeof(response)) == 0)
        std::printf("[SIMPLON API] <- %s", response);

    if (dcu.getStatus("temperature", response, sizeof(response)) == 0) {
        double t = 0.0;
        if (jsonValueNumber(response, &t) == 0)
            std::printf("[SIMPLON API] <- %.1f deg\n", t);
        else
            printStatusLine("[SIMPLON API] <- temperature raw: ", response);
    }
    if (dcu.getStatus("humidity", response, sizeof(response)) == 0) {
        double h = 0.0;
        if (jsonValueNumber(response, &h) == 0)
            std::printf("[SIMPLON API] <- %.1f %%\n", h);
        else
            printStatusLine("[SIMPLON API] <- humidity raw: ", response);
    }

    // =============================================================================
    // DATA ACQUISITION INTERFACES (stream v2 / CBOR; consumer runs in another process)
    // =============================================================================
    dcu.setMonitorConfig("mode", "disabled");
    dcu.setFilewriterConfig("mode", "disabled");
    dcu.setStreamConfig("mode", "enabled");
    dcu.setStreamConfig("format", "cbor");
    dcu.setStreamConfig("header_detail", "all");

    // =============================================================================
    // ARM (software trigger runs in software_trigger_detector)
    // =============================================================================
    std::printf("Arming detector...\n");
    if (dcu.sendCommand("arm") != 0) {
        std::fprintf(stderr, "arm failed\n");
        return 1;
    }

    std::printf("Armed. Run: software_trigger_detector");
    if (host != kDefaultHost)
        std::printf(" %s", host);
    std::printf(" -n %d", number_of_triggers);
    std::printf("  then: wait_idle_and_disarm_detector");
    if (host != kDefaultHost)
        std::printf(" %s", host);
    std::printf("\n");

    return 0;
}
