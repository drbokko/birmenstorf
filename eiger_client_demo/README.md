# EIGER client demo (`eiger_client_demo`)

Small examples that talk to a **DECTRIS EIGER** detector control unit (**DCU**) over its **HTTP REST API** (default API segment **1.8.0**). The C++ layer exposes a **C ABI** (`cpp/eiger_client.h`, `cpp/eiger_client.cpp`); the Python side uses **`DEigerClient`** (`python/DEigerClient/`).

The main walkthrough below follows **`simple_acquisition_with_stream2`**: one **software-triggered** batch (**1000** frames in the default settings), **one** energy threshold, **stream v2** with **CBOR**, monitor and filewriter off. **Receiving** the stream is **not** implemented in these scripts—start your stream consumer in another process before arming if you need every frame.

**Disclaimer:** this is a **single-trigger** demo (`ntrigger = 1`, `nimages` frames per trigger). For continuous acquisition, increase **`ntrigger`** (and align detector settings). The **`trigger`** command can be sent from **another process** if you coordinate **arm** / **disarm** and timing.

---

## Repository layout

| Path | Role |
|------|------|
| `cpp/eiger_client.h`, `cpp/eiger_client.cpp` | C API: GET status, PUT commands, PUT detector/stream/monitor/filewriter config |
| `cpp/eiger_session.hpp` | Thin C++ wrapper holding host/port |
| `cpp/simple_acquisition_with_stream2.cpp` | End-to-end demo (builds to `simple_acquisition_with_stream2`) |
| `python/DEigerClient/DEigerClient.py` | Python client for the same REST surface |
| `python/simple_acquisition_with_stream2.py` | Same demo as the C++ binary |
| `Makefile` | Builds the C++ demo on Linux (needs **libcurl** via `pkg-config`) |

---

## Build and run

**C++** (from this directory):

```bash
make
./simple_acquisition_with_stream2 [HOST]
# optional: ./simple_acquisition_with_stream2 --force-init 169.254.254.1
```

**Python**:

```bash
cd python
python3 simple_acquisition_with_stream2.py
```

Edit **`DCU_IP`** / **`DCU_PORT`** in the Python script, or pass **`HOST`** to the C++ binary (default `169.254.254.1`). With **`verbose=True`** (Python) and **`eiger_set_http_trace(stdout)`** (C++), each request to the DCU is printed (**method**, **URL**, **PUT body**).

Override the API version if needed: environment variable **`EIGER_API_VERSION`** (e.g. `1.6.0`).

---

## Step-by-step: `simple_acquisition_with_stream2`

The DCU expects a clear order: **configure** while safe, then **arm**, then **trigger**; **disarm** after the run. The tables below show the same step in **Python** and **C++** side by side.

### 1. Connect to the DCU

Create the client and buffers. Enable logging so every HTTP call to the DCU is visible on the console.

<table width="100%">
<tr>
<th align="left" width="50%">Python — <code>python/simple_acquisition_with_stream2.py</code></th>
<th align="left" width="50%">C++ — <code>cpp/simple_acquisition_with_stream2.cpp</code></th>
</tr>
<tr valign="top">
<td><pre lang="python"><code>c = DEigerClient.DEigerClient(host=DCU_IP, port=DCU_PORT, verbose=True)</code></pre></td>
<td><pre lang="cpp"><code>eiger_set_http_trace(stdout); // log HTTP method, URL, PUT body
EigerSession dcu(host, kPort);

char response[EIGER_CLIENT_RESPONSE_MAX];
char buf[64];</code></pre></td>
</tr>
</table>

### 2. Initialize when the detector is not idle

The DCU must be in a known state before applying configuration. Both scripts read **`state`**; if it is not **`idle`** (or you force initialization), they send the **`initialize`** command.

<table width="100%">
<tr>
<th align="left" width="50%">Python — <code>python/simple_acquisition_with_stream2.py</code></th>
<th align="left" width="50%">C++ — <code>cpp/simple_acquisition_with_stream2.cpp</code></th>
</tr>
<tr valign="top">
<td><pre lang="python"><code>print(f"Detector status:\t\t\t{c.detectorStatus('state')['value']}")
# Initialize if forced or not idle
if force_initialization or c.detectorStatus('state')['value'] != 'idle':
    print("Initializing detector...")
    c.sendDetectorCommand("initialize")</code></pre></td>
<td><pre lang="cpp"><code>if (dcu.getStatus("state", response, sizeof(response)) != 0) {
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
}</code></pre></td>
</tr>
</table>

### 3. Detector configuration

**Detector configuration** sets counting mode, corrections, **one** active threshold (second disabled), and acquisition timing: **`count_time`**, **`frame_time`**, **`nimages`**, **`ntrigger`**. Python passes native values; the C API expects JSON-shaped strings for each **`value`** (e.g. booleans as `false`/`true`, strings quoted).

<table width="100%">
<tr>
<th align="left" width="50%">Python — <code>python/simple_acquisition_with_stream2.py</code></th>
<th align="left" width="50%">C++ — <code>cpp/simple_acquisition_with_stream2.cpp</code></th>
</tr>
<tr valign="top">
<td><pre lang="python"><code># Usual settings for polychromatic beam
c.setDetectorConfig("countrate_correction_applied", False)
c.setDetectorConfig('retrigger', False)
c.setDetectorConfig('counting_mode', 'normal')
c.setDetectorConfig("flatfield_correction_applied", False)
c.setDetectorConfig('virtual_pixel_correction_applied', True)
c.setDetectorConfig('mask_to_zero', True)
c.setDetectorConfig('auto_summation', False)

# Thresholds and timing
for i, th in enumerate(thresholds, start=1):
    c.setDetectorConfig(f"threshold/{i}/mode", 'enabled')
    c.setDetectorConfig(f"threshold/{i}/energy", th)
if len(thresholds) &lt; 2:
    c.setDetectorConfig("threshold/2/mode", 'disabled')
c.setDetectorConfig("count_time", exposure_time)
c.setDetectorConfig('frame_time', exposure_time + sleep_time)
c.setDetectorConfig("nimages", number_of_images)
c.setDetectorConfig("ntrigger", number_of_triggers)</code></pre></td>
<td><pre lang="cpp"><code>// Usual settings for polychromatic beam
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
dcu.setDetectorConfig("ntrigger", buf);</code></pre></td>
</tr>
</table>

### 4. Housekeeping (parameter monitoring)

After configuration, both scripts **read** status endpoints useful for operations: **high voltage**, **temperature**, **humidity**, and they confirm **count** / **frame** time. This is **monitoring** only—no new settings.

<table width="100%">
<tr>
<th align="left" width="50%">Python — <code>python/simple_acquisition_with_stream2.py</code></th>
<th align="left" width="50%">C++ — <code>cpp/simple_acquisition_with_stream2.cpp</code></th>
</tr>
<tr valign="top">
<td><pre lang="python"><code>print(f"High voltage status:\t\t{c.detectorStatus('high_voltage/state')['value']}")
print(f"Temperature:\t\t\t{c.detectorStatus('temperature')['value']:.1f} deg")
print(f"Humidity:\t\t\t{c.detectorStatus('humidity')['value']:.1f} %")
print(f'Count time= {c.detectorConfig("count_time")["value"]}')
print(f'Frame time= {c.detectorConfig("frame_time")["value"]}')</code></pre></td>
<td><pre lang="cpp"><code>// After configuration check : high voltage, temperature, humidity
if (dcu.getStatus("high_voltage/state", response, sizeof(response)) == 0)
    printStatusLine("High voltage status:\t\t", response);
if (dcu.getStatus("temperature", response, sizeof(response)) == 0) {
    double t = 0.0;
    if (jsonValueNumber(response, &amp;t) == 0)
        std::printf("Temperature:\t\t\t%.1f deg\n", t);
    else
        printStatusLine("Temperature:\t\t\t", response);
}
if (dcu.getStatus("humidity", response, sizeof(response)) == 0) {
    double h = 0.0;
    if (jsonValueNumber(response, &amp;h) == 0)
        std::printf("Humidity:\t\t\t%.1f %%\n", h);
    else
        printStatusLine("Humidity:\t\t\t", response);
}

std::printf("Count time= %.9f s\n", exposure_time);
std::printf("Frame time= %.9f s\n", exposure_time + sleep_time);</code></pre></td>
</tr>
</table>

### 5. Data path configuration (stream consumer)

**Monitor** and **filewriter** are turned **off**; **stream** is **enabled** with **`format = cbor`** and full **`header_detail`**. This configures what the DCU **sends** on the stream v2 interface. Your **data consumer** (another process or tool) must connect to that stream and decode **CBOR**; it is **not** part of these scripts.

<table width="100%">
<tr>
<th align="left" width="50%">Python — <code>python/simple_acquisition_with_stream2.py</code></th>
<th align="left" width="50%">C++ — <code>cpp/simple_acquisition_with_stream2.cpp</code></th>
</tr>
<tr valign="top">
<td><pre lang="python"><code>c.setMonitorConfig("mode", "disabled")
c.setFileWriterConfig('mode', "disabled")
c.setStreamConfig('mode', "enabled")
c.setStreamConfig('format', 'cbor')
c.setStreamConfig('header_detail', 'all')</code></pre></td>
<td><pre lang="cpp"><code>dcu.setMonitorConfig("mode", "disabled");
dcu.setFilewriterConfig("mode", "\"disabled\"");
dcu.setStreamConfig("mode", "\"enabled\"");
dcu.setStreamConfig("format", "\"cbor\"");
dcu.setStreamConfig("header_detail", "\"all\"");</code></pre></td>
</tr>
</table>

### 6. Arming

**Arm** puts the detector into a state where it can accept **trigger**. Until **`arm`** succeeds, **`trigger`** must not be relied on.

<table width="100%">
<tr>
<th align="left" width="50%">Python — <code>python/simple_acquisition_with_stream2.py</code></th>
<th align="left" width="50%">C++ — <code>cpp/simple_acquisition_with_stream2.cpp</code></th>
</tr>
<tr valign="top">
<td><pre lang="python"><code>print("Acquiring data...")
c.sendDetectorCommand('arm')</code></pre></td>
<td><pre lang="cpp"><code>std::printf("Acquiring data...\n");
if (dcu.sendCommand("arm") != 0) {
    std::fprintf(stderr, "arm failed\n");
    return 1;
}</code></pre></td>
</tr>
</table>

### 7. Triggering

**Software trigger**: each **`trigger`** command starts acquisition according to **`nimages`** / **`ntrigger`**. You can **omit** this loop and send **`trigger`** from **another program** if the sequence is coordinated. On **`trigger`** failure, the C++ demo **disarms** and exits.

<table width="100%">
<tr>
<th align="left" width="50%">Python — <code>python/simple_acquisition_with_stream2.py</code></th>
<th align="left" width="50%">C++ — <code>cpp/simple_acquisition_with_stream2.cpp</code></th>
</tr>
<tr valign="top">
<td><pre lang="python"><code>for i in range(number_of_triggers):
    print(f"Triggering image {i+1}/{number_of_triggers}...")
    c.sendDetectorCommand('trigger')</code></pre></td>
<td><pre lang="cpp"><code>for (int i = 0; i &lt; number_of_triggers; i++) {
    std::printf("Triggering image %d/%d...\n", i + 1, number_of_triggers);
    if (dcu.sendCommand("trigger") != 0) {
        std::fprintf(stderr, "trigger failed\n");
        dcu.sendCommand("disarm");
        return 1;
    }
}</code></pre></td>
</tr>
</table>

### 8. Wait for idle, then disarm

Wait until **`state`** returns to **`idle`** so the acquisition is finished, then **disarm** to leave the detector in a clean state. The C++ code polls every **100 ms**; Python uses the same interval.

<table width="100%">
<tr>
<th align="left" width="50%">Python — <code>python/simple_acquisition_with_stream2.py</code></th>
<th align="left" width="50%">C++ — <code>cpp/simple_acquisition_with_stream2.cpp</code></th>
</tr>
<tr valign="top">
<td><pre lang="python"><code>while c.detectorStatus('state')['value'] != 'idle':
    time.sleep(0.1)
c.sendDetectorCommand("disarm")</code></pre></td>
<td><pre lang="cpp"><code>for (;;) {
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
    std::fprintf(stderr, "Warning: disarm failed\n");</code></pre></td>
</tr>
</table>

---

## C API reference (summary)

| Operation | C function | Typical use |
|-----------|------------|-------------|
| Read status | `eiger_get_status(host, port, path, buf, size)` | `state`, `temperature`, `humidity`, `high_voltage/state` |
| Command | `eiger_send_command(host, port, name)` | `initialize`, `arm`, `trigger`, `disarm` |
| Detector config | `eiger_set_detector_config(host, port, param, value_json)` | PUT `{"value": ...}` |
| Stream / monitor / filewriter | `eiger_set_stream_config`, `eiger_set_monitor_config`, `eiger_set_filewriter_config` | Enable stream, CBOR, etc. |

Failed HTTP requests are reported on **stderr**. On **Windows**, the implementation uses **WinHTTP**; on **Linux** with **libcurl**, request tracing is enabled with **`eiger_set_http_trace(FILE *fp)`** (`NULL` to disable).

### Minimal C example

```c
#include "eiger_client.h"

char response[EIGER_CLIENT_RESPONSE_MAX];
const char *host = "172.31.1.1";
int port = 80;

eiger_get_status(host, port, "state", response, sizeof(response));
eiger_set_detector_config(host, port, "counting_mode", "\"normal\"");
eiger_set_stream_config(host, port, "mode", "\"enabled\"");
eiger_send_command(host, port, "arm");
eiger_send_command(host, port, "trigger");
/* Wait until GET status/state reports idle, then: */
eiger_send_command(host, port, "disarm");
```

For the full sequence (including **wait for idle** before **disarm**), see **`cpp/simple_acquisition_with_stream2.cpp`** and **`python/simple_acquisition_with_stream2.py`**.
