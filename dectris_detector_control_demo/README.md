# DECTRIS detector control demo (`dectris_detector_control_demo`)

Small **C++** and **Python** examples that drive a **DECTRIS EIGER** detector control unit (**DCU**) over **HTTP REST** (Simplon-style API: detector, monitor, filewriter, stream modules; commands `initialize`, `arm`, `trigger`, `disarm`). The walkthrough acquisition uses **stream v2** with **CBOR**, one software **trigger**, and one energy threshold.

**Simplon API rules (summary).** Respect detector **state**; call `initialize` when needed; `arm` before `trigger`; `disarm` to leave the armed path. Subsystems use separate URL prefixes (`/detector/…`, `/stream/…`, …); config `PUT` bodies use a JSON `value` field. For authoritative rules and parameters, use the **SIMPLON API Reference** for your firmware.

**Detector configuration checklist** (both languages): **off** — `countrate_correction_applied`, `retrigger`, `flatfield_correction_applied`, `auto_summation`; **on** — `virtual_pixel_correction_applied`, `mask_to_zero`; set `counting_mode` as a **string** (here `normal`); set thresholds and `count_time` / `frame_time` / `nimages` / `ntrigger`; **do not** set `photon_energy` (can disturb thresholds); **monitor** and **filewriter** **off**, **stream** **on**.

`simple_acquisition_with_stream2` does **not** include a stream **consumer**—start your receiver in **another process** before arming if you cannot miss frames.

---

## Repository layout

| Path | Role |
|------|------|
| `Makefile` | Builds the C++ demo (`libcurl` on Linux via `pkg-config`) |
| `cpp/eiger_client.h`, `cpp/eiger_client.cpp` | C ABI: `GET` status, `PUT` commands, `PUT` config for detector / stream / filewriter; monitor uses a dedicated body builder |
| `cpp/eiger_session.hpp` | C++ `EigerSession`: host/port, `getStatus`, `sendCommand`, typed `setDetectorConfig` / `setStreamConfig` / `setFilewriterConfig` |
| `cpp/simple_acquisition_with_stream2.cpp` | End-to-end demo executable |
| `python/DEigerClient/` | Python client for the same REST surface |
| `python/simple_acquisition_with_stream2.py` | Same sequence as the C++ program |

---

## C++ stack: `eiger_client` and `EigerSession`

The C layer builds request bodies like `{"value": …}` by splicing a **JSON value fragment** (see `build_value_body` in `eiger_client.cpp`). **EigerSession** turns C++ values into that fragment so call sites stay simple:

- `bool` → `true` / `false`
- `int` / `double` → decimal text
- `const char*` → a JSON **string** (quotes and minimal `\` / `"` escaping)

**Monitor** config still uses `setMonitorConfig(param, value)` with a plain string (the C code wraps it in `{"value": "<value>"}`); use `"disabled"`, not a JSON-quoted blob.

**Trap:** `setDetectorConfig("x", "false")` uses the **string** overload and sends a JSON string `"false"`, not a boolean. Use `setDetectorConfig("x", false)` for booleans.

Enable request logging with `eiger_set_http_trace(stdout)` (method, URL, `PUT` body).

---

## Build and run

From this directory:

**C++**

```bash
make
./simple_acquisition_with_stream2 [HOST]
# ./simple_acquisition_with_stream2 --force-init 169.254.254.1
```

**Python**

```bash
cd python && python3 simple_acquisition_with_stream2.py
```

Set `EIGER_API_VERSION` if the DCU uses an API segment other than `1.8.0`.

---

## Code walkthrough (C++): `cpp/simple_acquisition_with_stream2.cpp`

The following follows `main` top to bottom—the order of operations the DCU sees.

### Helpers (before `main`)

Local helpers parse JSON status text from `getStatus`: `stateIsIdle`, `jsonValueNumber`, `printStatusLine`. They do not add HTTP calls.

```cpp
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
```

### 1. User input and connection

**Host** and **port** come from defaults and arguments (`HOST`, `--force-init`). Acquisition constants: `threshold_ev`, `number_of_images`, `number_of_triggers`, `exposure_time`, `sleep_time`.

Tracing is enabled; `EigerSession` is constructed; a buffer is reserved for `getStatus` responses.

*Excerpt from `cpp/simple_acquisition_with_stream2.cpp` (start of `main`).*

```cpp
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
    // Data acquisition (software trigger; ntrigger=1 → one batch of nimages frames; one threshold)
    const int threshold_ev = 15000;
    const int number_of_images = 1000;
    const int number_of_triggers = 1; // Each trigger acquires number_of_images frames
    const double exposure_time = 1.0 / 1000.0; // Count time per image [s] (e.g. 1/fps)
    const double sleep_time = 0.0; // Extra delay per frame [s]; frame_time = exposure + sleep

    // =============================================================================
    // CONNECT TO DETECTOR
    // =============================================================================
    eiger_set_http_trace(stdout); // log every HTTP method, URL, and PUT body to the DCU
    EigerSession dcu(host, kPort);

    char response[EIGER_CLIENT_RESPONSE_MAX];
```

### 2. Initialize

`GET` `state`; if not **idle** (or `--force-init`), `PUT` `initialize`.

```cpp
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
```

### 3. Detector configuration

`EigerSession::setDetectorConfig` overloads format the JSON `value` for `eiger_set_detector_config` (see `eiger_session.hpp`).

**Important — same semantics as Python:**

- **Disable** (bool `false`): `countrate_correction_applied`, `retrigger`, `flatfield_correction_applied`, `auto_summation`.
- **Enable** (bool `true`): `virtual_pixel_correction_applied`, `mask_to_zero`.
- **String** mode: `counting_mode` → `"normal"` in this demo.
- **Then** thresholds and timing: `threshold/…`, `count_time`, `frame_time`, `nimages`, `ntrigger`.
- **Do not** set `photon_energy`.
- **Data path** in step 5: monitor/filewriter off, stream on.

```cpp
    // =============================================================================
    // CONFIGURATION  (IMPORTANT)
    // =============================================================================
    // Detector (Simplon-style layout for this stream demo):
    //   Disable: countrate_correction_applied, retrigger, flatfield_correction_applied,
    //            auto_summation.
    //   counting_mode is a string parameter (not a bool): set explicitly as required;
    //   this demo uses "normal".
    //   Enable: virtual_pixel_correction_applied, mask_to_zero.
    //   Then: threshold mode/energy, count_time, frame_time, nimages, ntrigger.
    //   Do NOT set photon_energy: it can overwrite threshold-related settings.
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
    dcu.setDetectorConfig("ntrigger", number_of_triggers);
```

### 4. Housekeeping (parameter monitoring)

Read-only status: high voltage, temperature, humidity; echo count/frame time. No extra detector `PUT`s.

```cpp
    // After configuration check : high voltage, temperature, humidity
    if (dcu.getStatus("high_voltage/state", response, sizeof(response)) == 0)
        printStatusLine("High voltage status: ", response);
    if (dcu.getStatus("temperature", response, sizeof(response)) == 0) {
        double t = 0.0;
        if (jsonValueNumber(response, &t) == 0)
            std::printf("Temperature: %.1f deg\n", t);
        else
            printStatusLine("Temperature: ", response);
    }
    if (dcu.getStatus("humidity", response, sizeof(response)) == 0) {
        double h = 0.0;
        if (jsonValueNumber(response, &h) == 0)
            std::printf("Humidity: %.1f %%\n", h);
        else
            printStatusLine("Humidity: ", response);
    }

    std::printf("Count time= %.9f s\n", exposure_time);
    std::printf("Frame time= %.9f s\n", exposure_time + sleep_time);
```

### 5. Data interfaces (stream configuration)

Monitor off, filewriter off, stream on with `format` `cbor` and `header_detail` `all`. The Data Consumer (the Stream2 receiver) runs elsewhere.

```cpp
    // =============================================================================
    // DATA ACQUISITION INTERFACES (stream v2 / CBOR; the data consumer might run in another process or another machine)
    // =============================================================================
    dcu.setMonitorConfig("mode", "disabled");
    dcu.setFilewriterConfig("mode", "disabled");
    dcu.setStreamConfig("mode", "enabled");
    dcu.setStreamConfig("format", "cbor");
    dcu.setStreamConfig("header_detail", "all");
```

### 6. Arming

`arm` must succeed before `trigger`.

```cpp
    // =============================================================================
    // RUN ACQUISITION (arm before trigger; trigger may run in another process)
    // =============================================================================
    std::printf("Acquiring data...\n");
    if (dcu.sendCommand("arm") != 0) {
        std::fprintf(stderr, "arm failed\n");
        return 1;
    }
```

### 7. Triggering

Software `trigger` loop; optional to move `trigger` to another process with coordinated `arm` / `disarm`. On error, `disarm` and exit.

```cpp
    // Software trigger from this process; skip loop if another process sends trigger
    for (int i = 0; i < number_of_triggers; i++) {
        std::printf("Triggering image %d/%d...\n", i + 1, number_of_triggers);
        if (dcu.sendCommand("trigger") != 0) {
            std::fprintf(stderr, "trigger failed\n");
            dcu.sendCommand("disarm");
            return 1;
        }
    }
```

### 8. Wait for idle, then disarm

Poll `state` until **idle**, then `disarm` (100 ms between `GET`s).

```cpp
    // Disarm only after acquisition has finished (state returns to idle), like the Python demo.
    for (;;) {
        if (dcu.getStatus("state", response, sizeof(response)) != 0) {
            std::fprintf(stderr, "Failed to read state while waiting for idle\n");
            dcu.sendCommand("disarm");
            return 1;
        }
        if (stateIsIdle(response))
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (dcu.sendCommand("disarm") != 0)
        std::fprintf(stderr, "Warning: disarm failed\n");

    return 0;
}
```

---

## Python

`python/simple_acquisition_with_stream2.py` mirrors the C++ sequence: `DEigerClient` with `verbose=True`, the same **CONFIGURATION (IMPORTANT)** comments, housekeeping reads, monitor/filewriter/stream setup, `arm` / `trigger` / wait for `idle` / `disarm`. `DEigerClient.setDetectorConfig` takes native Python types (booleans, numbers, strings) and serializes JSON similarly—compare with `EigerSession` in C++.
