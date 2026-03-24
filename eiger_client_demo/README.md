# EIGER client demo

This directory contains a small **C++** program and a matching **Python** script that configure a **DECTRIS EIGER** detector control unit (**DCU**) and run a short acquisition over **stream v2** (**CBOR**). They use the DCU’s **HTTP REST** surface—the same one described in the **SIMPLON API Reference** from DECTRIS (detector, monitor, filewriter, and stream subsystems, status and config parameters, commands such as `initialize`, `arm`, `trigger`, `disarm`).

**Simplon API rules (summary).** In normal operation you respect the detector **state**: only apply configuration when allowed for that state; use `**initialize`** when the system is not ready; `**arm**` before the detector will accept `**trigger**`; use `**disarm**` to leave the armed path. Subsystems are separate URLs (`/detector/…`, `/stream/…`, etc.); each PUT carries a JSON body with a `**value**` field as in the reference. This demo follows that ordering; for exact preconditions, allowed parameters, and edge cases, use the official **SIMPLON** documentation for your firmware version.

**Detector configuration checklist** used in both scripts: turn **off** count-rate correction, **retrigger**, flat-field correction, and **auto_summation**; set `**counting_mode`** as a **string** (here `**normal`**); turn **on** virtual-pixel correction and **mask_to_zero**; set thresholds and timing; **avoid `photon_energy`** so thresholds are not overwritten; then **monitor** and **filewriter** **off**, **stream** **on** (step 3 and step 5 below spell this out).

The program `**simple_acquisition_with_stream2`** does **not** implement a stream **receiver**—only DCU-side stream configuration. Run your consumer in **another process** before arming if you must not miss data.

---

## Build and run

**C++** (needs **libcurl** on Linux, see `pkg-config libcurl`):

```bash
make
./simple_acquisition_with_stream2 [HOST]
# ./simple_acquisition_with_stream2 --force-init 169.254.254.1
```

**Python:**

```bash
cd python && python3 simple_acquisition_with_stream2.py
```

Set `**EIGER_API_VERSION**` if your DCU uses an API segment other than the default **1.8.0**.

---

## Code walkthrough (C++): `cpp/simple_acquisition_with_stream2.cpp`

The rest of this section follows `**main()`** from top to bottom: same order the DCU sees requests.

### Helpers (before `main`)

Small utilities parse JSON status snippets returned by the DCU: `**stateIsIdle**` looks for an `"idle"` value, `**jsonValueNumber**` reads a numeric `**value**` for temperature/humidity, `**printStatusLine**` prints raw JSON when needed. They are not extra HTTP calls; they only interpret buffers filled by `**getStatus**`.

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

**Host and port** come from constants and arguments (`HOST`, optional `**--force-init`**). Acquisition parameters are fixed in code: one threshold, `**number_of_images**`, `**number_of_triggers**`, `**count_time**` / `**frame_time**`.

Then the program enables **HTTP tracing** (every method, URL, and PUT body on **stdout**) and constructs `**EigerSession`**, which holds `**host**` / `**port**` and forwards to the C client. Response and scratch buffers are allocated once.

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
    char buf[64];
```

### 2. Initialize

First **GET** of detector `**state`**. If the DCU is not **idle**, or `**--force-init`** was passed, the program sends the `**initialize**` command so later configuration runs against a known state (per Simplon state rules).

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

**PUT** calls set detector **config** parameters. Values are JSON-compatible strings in C++ (booleans and quoted strings).

**Important — configure the detector like this (same in Python and C++):**

- **Disable** the following: `countrate_correction_applied`, `retrigger`, `flatfield_correction_applied`, `auto_summation` (all off / `false` in this demo).
- **Enable** the following:`virtual_pixel_correction_applied`, `mask_to_zero`.
- Only then **Set:** threshold **mode** and **energy**, `**count_time`**, `**frame_time**`, `**nimages**`, `**ntrigger**`.
- Remember **do not set** `photon_energy` here: on many setups it can **overwrite or conflict with threshold** configuration.
- **Enable** only the data interfaces you need (see step 5):
  - monitor **off**
  - filewriter **off**
  - stream **on**.
- **Attention** the `counting_mode` is not a boolean — set the **mode string** your measurement needs; this demo uses `**normal`** (do not read “disable counting_mode” as a boolean flag).

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
    dcu.setDetectorConfig("countrate_correction_applied", "false");
    dcu.setDetectorConfig("retrigger", "false");
    dcu.setDetectorConfig("counting_mode", "\"normal\""); // see IMPORTANT: mode string, not "disabled"
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
```

### 4. Housekeeping (parameter monitoring)

After configuration, the code **reads** status only: **high voltage**, **temperature**, **humidity**, and prints configured **count** / **frame** time. No further detector **PUT**s here—this is operational readout aligned with Simplon **status** paths.

```cpp
    // After configuration check : high voltage, temperature, humidity
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
```

### 5. Data interfaces (stream configuration)

This matches the **IMPORTANT** checklist: **monitor disabled**, **filewriter disabled**, **stream enabled**. Here the stream uses `**format = cbor`** and `**header_detail = all**` for stream v2–style delivery. This is only the **DCU** side; **receiving** and decoding CBOR frames runs in **another process**.

```cpp
    // =============================================================================
    // DATA ACQUISITION INTERFACES (stream v2 / CBOR; consumer runs in another process)
    // =============================================================================
    dcu.setMonitorConfig("mode", "disabled");
    dcu.setFilewriterConfig("mode", "\"disabled\"");
    dcu.setStreamConfig("mode", "\"enabled\"");
    dcu.setStreamConfig("format", "\"cbor\"");
    dcu.setStreamConfig("header_detail", "\"all\"");
```

### 6. Arming

`**arm**` transitions the detector into a state where it can accept **software `trigger`** (Simplon command sequence). Failure aborts the program without disarming if arm never succeeded.

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

A loop sends `**trigger**` once per `**number_of_triggers**`. Each trigger acquires `**number_of_images**` frames as configured. You may move `**trigger**` to another process if you keep the same **arm** / **disarm** contract. On failure, the code **disarms** and exits.

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

The program polls `**state**` until it is **idle** again (acquisition finished), then `**disarm`**. Polling uses a **100 ms** sleep between **GET**s. `**disarm`** failure is reported but does not change the exit code.

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

`**python/simple_acquisition_with_stream2.py**` performs the **same steps in the same order** as the C++program, including the **CONFIGURATION (IMPORTANT)** comment block (disable/enable list, **no `photon_energy`**, monitor/filewriter off, stream on). Use it when you prefer the `**DEigerClient**` API; behavior matches the C++ flow above.