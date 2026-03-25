# DECTRIS detector control demo (`dectris_detector_control_demo`)

**C++** and **Python** examples for a **DECTRIS EIGER** **DCU** over **HTTP REST** (Simplon-style API). The C++ flow is three steps: **configure + `arm`**, then **software `trigger`**, then **wait for `idle` + `disarm`**. **Python** keeps one script for the full sequence.

**Simplon API rules (summary).** Respect detector **state**; call `initialize` when needed; `arm` before `trigger`; `disarm` to leave the armed path. Subsystems use separate URL prefixes (`/detector/…`, `/stream/…`, …); config `PUT` bodies use a JSON `value` field. For authoritative rules, use the **SIMPLON API Reference** for your firmware.

**Detector configuration checklist**: **off** — `countrate_correction_applied`, `retrigger`, `flatfield_correction_applied`, `auto_summation`; **on** — `virtual_pixel_correction_applied`, `mask_to_zero`; `counting_mode` as a **string** (here `normal`); thresholds and `count_time` / `frame_time` / `nimages` / `ntrigger`; **do not** set `photon_energy`; **monitor** / **filewriter** **off**, **stream** **on**.

Neither C++ nor Python includes a stream **consumer**—run your receiver in **another process** before arming if you cannot miss frames.

---

## Repository layout

| Path | Role |
|------|------|
| `Makefile` | Builds three C++ binaries (`libcurl` on Linux) |
| `cpp/eiger_client.h`, `cpp/eiger_client.cpp` | C ABI for REST calls |
| `cpp/eiger_session.hpp` | `EigerSession`: typed `setDetectorConfig` / stream / filewriter / monitor |
| `cpp/connect_and_configure_and_arm_detector.cpp` | Part 1: configure + housekeeping + stream + `arm` |
| `cpp/software_trigger_detector.cpp` | Part 2: `trigger` loop only |
| `cpp/wait_idle_and_disarm_detector.cpp` | Part 3: poll `state` until idle, `disarm` |
| `python/DEigerClient/` | Python client |
| `python/simple_acquisition_with_stream2.py` | Full sequence in one script |

---

## C++ stack: `eiger_client` and `EigerSession`

The C layer builds `{"value": …}` via `build_value_body` in `eiger_client.cpp`. **EigerSession** maps `bool`, `int`, `double`, and string literals to JSON value fragments. **Monitor** uses plain strings for `setMonitorConfig`. Use `setDetectorConfig(..., false)` for booleans, not `"false"` as a string. **`eiger_set_http_trace(stdout)`** prefixes outgoing requests with `[SIMPLON API] ->`; the C++ programs **`printf`** selected status lines as **`[SIMPLON API] <-`** where noted in source.

---

## Build and run

**C++**

```bash
make
./connect_and_configure_and_arm_detector [--force-init] [HOST]
./software_trigger_detector [-n N] [HOST]   # -n must match ntrigger in part 1 source
./wait_idle_and_disarm_detector [HOST]
```

**Python**

```bash
cd python && python3 simple_acquisition_with_stream2.py
```

Set `EIGER_API_VERSION` if the DCU uses an API segment other than `1.8.0`.

---

## Code walkthrough (C++ part 1): `cpp/connect_and_configure_and_arm_detector.cpp`

### Helpers (before `main`)

`stateIsIdle`, `jsonValueNumber`, `printStatusLine` parse status JSON from `getStatus`; no extra HTTP calls.

### Sections in `main`

1. **USER INPUT** — `HOST`, `--force-init`; constants `threshold_ev`, `number_of_images`, `number_of_triggers`, timing.
2. **CONNECT TO DETECTOR** — `eiger_set_http_trace(stdout)`, `EigerSession`, response buffer.
3. **INITIALIZE** — `GET` `state`; `[SIMPLON API] <-` full JSON; `initialize` if needed.
4. **CONFIGURATION (IMPORTANT)** — long comment block + `setDetectorConfig` / thresholds / `nimages` / `ntrigger`; `[SIMPLON API] ->` lines for configured count/frame time.
5. **Housekeeping** — `GET` high voltage, temperature, humidity; `[SIMPLON API] <-` (numeric or raw).
6. **DATA ACQUISITION INTERFACES** — monitor/filewriter off, stream CBOR.
7. **ARM** — `sendCommand("arm")`; prints how to run parts 2 and 3 (`-n` + `wait_idle_and_disarm_detector`).

---

## Code walkthrough (C++ part 2): `cpp/software_trigger_detector.cpp`

1. **USER INPUT** — `HOST`, `-n` (default 1).
2. **CONNECT** — trace + `EigerSession`.
3. **TRIGGER** — loop `sendCommand("trigger")`; on failure emergency `disarm` and exit; success prints hint to run part 3.

---

## Code walkthrough (C++ part 3): `cpp/wait_idle_and_disarm_detector.cpp`

1. **USER INPUT** — `HOST`.
2. **CONNECT** — trace + `EigerSession`, response buffer.
3. **Wait for idle** — poll `GET` `state`, **`[SIMPLON API] <-`** each response until idle.
4. **`disarm`** — `sendCommand("disarm")`.

---

## Python

`python/simple_acquisition_with_stream2.py` runs configure → housekeeping → stream → `arm` → `trigger` → wait `idle` → `disarm` in one process, with **`verbose=True`** for HTTP logging. Same **CONFIGURATION (IMPORTANT)** ideas as the C++ programs; compare with `EigerSession` typing.
