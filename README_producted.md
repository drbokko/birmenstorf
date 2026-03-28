# DECTRIS Detector Control Demo (`dectris_detector_control`)

DECTRIS Detector Control is built around the **producer / consumer** split, covering the control of producer side.

- **Producer (these C++ tools + DCU)** — Configure the EIGER **DCU** so it **pushes** detector data over **stream v2** (**CBOR**). The C++ tools only drive the REST **Simplon** API on the DCU. They do **not** receive or save frames.

The **Python** script is an optional **all-in-one** demo for the same REST sequence; it still does not implement a stream consumer.

---

## Philosophy: producer workflow (configure → trigger → disarm)

| Step | Program | What it does |
|------|---------|----------------|
| **1 — Configure and Arm** | `connect_and_configure_and_arm_detector` | Defines the **acquisition contract** on the DCU: corrections, thresholds, timing, **`nimages`** (frames **per** software trigger) and **`ntrigger`** via **`--nimages`** / **`--ntrigger`** (defaults **1000** / **100000**), stream **on** (CBOR), monitor/filewriter **off**. Then **`arm`**. **No** frames are emitted until you trigger. |
| **2 — Trigger (manual or external)** | `send_software_trigger` | You run this **when you want data**. Each **`trigger`** command tells the detector to acquire **`nimages`** frames; they appear on the **stream** for your **consumer** to collect. **`--ntrigger`** on this program must match the **`ntrigger`** you configured in step 1 (same host). You can think of step 1 as reserving “up to **`ntrigger`** bursts of **`nimages`** frames” and step 2 as issuing those bursts on demand. |
| **3 — Disarm (normal end)** | `wait_idle_and_disarm_detector` | After the last trigger of a run, wait until **`state`** is **`idle`**, then **`disarm`** so the DCU leaves the armed acquisition path cleanly. |
| **Stop acquisition now** | `disarm_detector` | Sends **`disarm`** immediately **without** waiting for idle. Use to **abort** or stop an armed sequence; the stream may end mid-series. Prefer **`wait_idle_and_disarm_detector`** after a completed run. |

Use **`--nimages`** and **`--ntrigger`** on `connect_and_configure_and_arm_detector` (defaults **1000** / **100000**) to match your experiment. **`send_software_trigger --ntrigger`** must equal that configured **`ntrigger`** (or the number of triggers you intend to send before teardown).

**Simplon API rules (summary).** Respect detector **state**; **`initialize`** when needed; **`arm`** before **`trigger`**; **`disarm`** after the run. Config **`PUT`** bodies use a JSON **`value`** field. See the **SIMPLON API Reference** for your firmware.

**Detector configuration checklist** (same idea in C++ and Python): turn **off** `countrate_correction_applied`, `retrigger`, `flatfield_correction_applied`, `auto_summation`; turn **on** `virtual_pixel_correction_applied`, `mask_to_zero`; set **`counting_mode`** as a string (e.g. `normal`); set thresholds and **`count_time`** / **`frame_time`** / **`nimages`** / **`ntrigger`**; **do not** set **`photon_energy`** if it would disturb thresholds; **monitor** / **filewriter** **off**, **stream** **on**.

---

## Repository layout

| Path | Role |
|------|------|
| `Makefile` | Builds C++ binaries (`libcurl` on Linux) |
| `cpp/eiger_client.*`, `cpp/eiger_session.hpp` | HTTP client + typed config helpers |
| `cpp/connect_and_configure_and_arm_detector.cpp` | Producer **setup**: configure stream + **`arm`** |
| `cpp/send_software_trigger.cpp` | Producer **manual fire**: **`trigger`** × **N** |
| `cpp/wait_idle_and_disarm_detector.cpp` | Producer **teardown**: wait **idle**, **`disarm`** |
| `cpp/disarm_detector.cpp` | **Immediate** **`disarm`** (stop acquisition without idle wait) |
| `python/` | `DEigerClient` + `simple_acquisition_with_stream2.py` (full REST sequence in one process) |
| `dectris_data_consumer/` | Stream v2 **consumer**: CMake project (`DectrisStream2Receiver_linux`, `acquire_and_save_stream`, …) |

---

## C++ stack (brief)

**`eiger_client`** builds JSON bodies `{"value": …}`. **`EigerSession`** overloads **`bool` / `int` / `double` / string** so call sites stay readable. **`eiger_set_http_trace(stdout)`** logs **`[SIMPLON API] ->`** on each request; the programs also print **`[SIMPLON API] <-`** for selected status reads.

---

## Build and run

```bash
make
```

**Typical producer workflow** (consumer already running if you must capture everything):

```bash
./connect_and_configure_and_arm_detector [--nimages N] [--ntrigger M] [--force-init] [HOST]
# … when ready to generate data …
./send_software_trigger [--ntrigger M] [HOST]   # M = ntrigger from step 1
./wait_idle_and_disarm_detector [HOST]

# To stop acquisition immediately (abort; no idle wait):
# ./disarm_detector [HOST]
```

Set **`EIGER_API_VERSION`** if the DCU API segment is not **`1.8.0`**.

**Python** (single-process reference, includes trigger + idle + disarm):

```bash
cd python && python3 simple_acquisition_with_stream2.py
```

---

## Walkthrough (producer scripts)

### Part 1 — `connect_and_configure_and_arm_detector.cpp`

Sets **`nimages`** / **`ntrigger`** in detector config (CLI **`--nimages`** / **`--ntrigger`** or defaults), enables **stream** (**CBOR**), **`arm`**. Prints the suggested **`send_software_trigger --ntrigger …`** line. Sections: **USER INPUT**, **CONNECT**, **INITIALIZE**, **CONFIGURATION (IMPORTANT)**, housekeeping **`[SIMPLON API] <-`**, **DATA ACQUISITION INTERFACES**, **ARM**.

### Part 2 — `send_software_trigger.cpp`

Sends **`N`** **`trigger`** commands only. On **`trigger`** failure it **`disarm`**s as an emergency recovery hint. After success, run part 3.

### Part 3 — `wait_idle_and_disarm_detector.cpp`

Polls **`state`** with **`[SIMPLON API] <-`** until **idle**, then **`disarm`**.

### `disarm_detector.cpp`

Sends **`disarm`** once, with no idle polling. Stops an armed acquisition path right away; use when you need to halt the run without waiting for exposures to finish.

---

## Python

**`simple_acquisition_with_stream2.py`** runs configure → housekeeping → stream → **`arm`** → **`trigger`** → wait **`idle`** → **`disarm`** in one process with **`verbose=True`**. It matches the **same Simplon checklist** as part 1 but does not split manual trigger/teardown; it does **not** replace a stream **consumer**.
