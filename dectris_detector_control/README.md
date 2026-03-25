# DECTRIS detector control demo (`dectris_detector_control_demo`)

This repo is built around a **producer / consumer** split:

- **Producer (these C++ tools + DCU)** — Configure the EIGER **DCU** so it **pushes** detector data over **stream v2** (**CBOR**). The three binaries only drive the REST **Simplon** API on the DCU. They do **not** receive or save frames.
- **Consumer (your responsibility)** — A **separate program** (or another machine) that connects to the detector’s stream endpoint, decodes **CBOR**, and stores or processes images. This repository includes a CMake-based receiver under **`dectris_data_consumer/`** (e.g. `DectrisStream2Receiver_linux`, **`acquire_and_save_stream`** for a fixed frame count and optional flatfield TIFF; see its `README.md`). Start the consumer **before** you care about not losing data (typically before **`arm`**, or at least before you run the **trigger** script).

The **Python** script is an optional **all-in-one** demo for the same REST sequence; it still does not implement a stream consumer.

---

## Philosophy: three C++ steps = producer side only

| Step | Program | What it does |
|------|---------|----------------|
| **1 — Configure and Arm** | `connect_and_configure_and_arm_detector` | Defines the **acquisition contract** on the DCU: corrections, thresholds, timing, **`nimages`** (frames **per** software trigger), **`ntrigger`** (how many triggers the armed sequence allows), stream **on** (CBOR), monitor/filewriter **off**. Then **`arm`**. **No** frames are emitted until you trigger. |
| **2 — Trigger (manual)** | `software_trigger_detector` | You run this **when you want data**. Each **`trigger`** command tells the detector to acquire **`nimages`** frames; they appear on the **stream** for your **consumer** to collect. **`-n`** on this program must match the **`ntrigger`** you configured in step 1 (same host). You can think of step 1 as reserving “up to **`ntrigger`** bursts of **`nimages`** frames” and step 2 as issuing those bursts on demand. |
| **3 — Disarm** | `wait_idle_and_disarm_detector` | After the last trigger of a run, wait until **`state`** is **`idle`**, then **`disarm`** so the DCU leaves the armed acquisition path cleanly. |

Edit **`number_of_images`** and **`number_of_triggers`** in `connect_and_configure_and_arm_detector.cpp` to match your experiment (example: **1000** images per trigger and a large **`ntrigger`** if you want many manual trigger batches under one arm). **`software_trigger_detector -n`** must equal the configured **`ntrigger`** for that arm cycle (or the number of triggers you intend to send before teardown).

**Simplon API rules (summary).** Respect detector **state**; **`initialize`** when needed; **`arm`** before **`trigger`**; **`disarm`** after the run. Config **`PUT`** bodies use a JSON **`value`** field. See the **SIMPLON API Reference** for your firmware.

**Detector configuration checklist** (same idea in C++ and Python): turn **off** `countrate_correction_applied`, `retrigger`, `flatfield_correction_applied`, `auto_summation`; turn **on** `virtual_pixel_correction_applied`, `mask_to_zero`; set **`counting_mode`** as a string (e.g. `normal`); set thresholds and **`count_time`** / **`frame_time`** / **`nimages`** / **`ntrigger`**; **do not** set **`photon_energy`** if it would disturb thresholds; **monitor** / **filewriter** **off**, **stream** **on**.

---

## Repository layout

| Path | Role |
|------|------|
| `Makefile` | Builds three C++ binaries (`libcurl` on Linux) |
| `cpp/eiger_client.*`, `cpp/eiger_session.hpp` | HTTP client + typed config helpers |
| `cpp/connect_and_configure_and_arm_detector.cpp` | Producer **setup**: configure stream + **`arm`** |
| `cpp/software_trigger_detector.cpp` | Producer **manual fire**: **`trigger`** × **N** |
| `cpp/wait_idle_and_disarm_detector.cpp` | Producer **teardown**: wait **idle**, **`disarm`** |
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
./connect_and_configure_and_arm_detector [--force-init] [HOST]
# … when ready to generate data …
./software_trigger_detector [-n N] [HOST]   # N = ntrigger from step 1 source
./wait_idle_and_disarm_detector [HOST]
```

Set **`EIGER_API_VERSION`** if the DCU API segment is not **`1.8.0`**.

**Python** (single-process reference, includes trigger + idle + disarm):

```bash
cd python && python3 simple_acquisition_with_stream2.py
```

---

## Walkthrough (producer scripts)

### Part 1 — `connect_and_configure_and_arm_detector.cpp`

Sets **`nimages`** / **`ntrigger`** in detector config, enables **stream** (**CBOR**), **`arm`**. Prints the suggested **`software_trigger_detector -n …`** and **`wait_idle_and_disarm_detector`** command line. Sections: **USER INPUT**, **CONNECT**, **INITIALIZE**, **CONFIGURATION (IMPORTANT)**, housekeeping **`[SIMPLON API] <-`**, **DATA ACQUISITION INTERFACES**, **ARM**.

### Part 2 — `software_trigger_detector.cpp`

Sends **`N`** **`trigger`** commands only. On **`trigger`** failure it **`disarm`**s as an emergency recovery hint. After success, run part 3.

### Part 3 — `wait_idle_and_disarm_detector.cpp`

Polls **`state`** with **`[SIMPLON API] <-`** until **idle**, then **`disarm`**.

---

## Python

**`simple_acquisition_with_stream2.py`** runs configure → housekeeping → stream → **`arm`** → **`trigger`** → wait **`idle`** → **`disarm`** in one process with **`verbose=True`**. It matches the **same Simplon checklist** as part 1 but does not split manual trigger/teardown; it does **not** replace a stream **consumer**.
