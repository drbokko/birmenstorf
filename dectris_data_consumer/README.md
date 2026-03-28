# DECTRIS Stream V2 Data Consumer — `dectris_data_consumer`

DECTRIS Stream V2 Data Consumer is built around the **producer / consumer** split, covering the collection of the data on the consumer side.

- **Consumer (your responsibility)** — A **separate program** (running eventually on another machine separate from the consumer) that connects to the detector’s stream endpoint, decodes **CBOR**, and stores or processes images. This repository includes a CMake-based receiver under **`dectris_data_consumer/`** (e.g. `DectrisStream2Receiver_linux`, **`acquire_and_save_stream`** for a fixed frame count and optional flatfield TIFF; see its `README.md`).

Start the consumer **before** starting the acquisition if you care about not losing data (typically before **`arm`**, or at least before you run the **trigger** script).
Often, if using the "ints" (internal trigger serie) trigger_mode, the consumer might control some parameter and eventually send the trigger. This is particularly true when actions of the produced data are required to be synchronized or affect the upcoming trigger, such in the case of the automatic acquisition of CdTe flatfield images during a longer continuous acquisition cycle.

C libraries and programs that **consume** DECTRIS Stream **V2** (CBOR over ZeroMQ) and write detector data (e.g. TIFF). They pair with the **producer** side in [`dectris_detector_control`](../dectris_detector_control/) (REST Simplon: configure, arm, trigger, disarm). Those files are derived from the official DECTRIS documentation library https://github.com/dectris/documentation

---

## Layout

| Location | Contents |
|----------|----------|
| `src/` | Sources for the static libraries (`stream2`, `stream2_helpers`) |
| `*.c` in this directory | Executable programs |
| `third_party/` | tinycbor, compression stack, and other CMake subprojects |

## C

### Requirements

- **Parser and helpers** use [tinycbor] and [dectris-compression] (pulled in via CMake / `third_party`).
- **Half-precision**: either a C11 compiler with ISO/IEC TS 18661-3 half-float support, or x86-64 with SSE2 and F16C.

### Libraries

CMake builds static targets **`stream2`** (parser) and **`stream2_helpers`** (linked by all C examples):

| Module | Role |
|--------|------|
| `src/stream2` | Stream V2 CBOR message parser |
| `src/stream2_common` | Clock, signals, shared helpers |
| `src/stream2_stats` | Receive / throughput statistics |
| `src/stream2_decompress` | Decompress image channels |
| `src/stream2_image_buffer` | In-memory buffering as on the wire; optional ZMQ zero-copy |
| `src/stream2_buffer_decode_stack` | Build a malloc-backed stack of decoded images (parallel decode); used by TIFF tools after receive |
| `src/tiff_writer` | Write TIFF from buffered images (single- or multi-threaded flush) |

### Programs

| Target | Description |
|--------|-------------|
| `stream2_dump` | Decode messages and print to stdout; useful to learn the protocol. |
| `stream2_buffer` | Receive and buffer images as on the wire, print stats; does not write files. |
| `stream2_buffer_decode` | Same as `stream2_buffer`, plus a compression summary (decompress-on-read for stats). |
| `DectrisStream2Receiver_linux` | Buffer as-received, then a pthread builds a decoded stack and writes TIFFs; `--threads` applies to decode and TIFF flush. |
| `acquire_and_save_stream` | Linux: **`--nimages` COUNT** then save (legacy **`-n`** / **`--images`**). Progress on **stderr**: IMAGE message count (fast) vs **fifo decoded** subimage count and **decode backlog** (may trail under compression). **`--generate-flatfield`**: per-frame TIFFs **and** mean flatfield. **`--generate-flatfield-only`**: flatfield only. **`--flatfield-file PATH`**: flatfield destination. **`--threads`**: TIFF writers only (decode is a single FIFO pipeline). Auto channel: `image` → `data` → unnamed → most common non-mask. |
| `stream2_bifurcator` | Relay: listen on one host/port, buffer, forward raw stream on another interface/port. |

**Windows-only targets** (when building on Windows): `DectrisStream2Demo_windows`, `start_stream_eigerclient`.

### Offline normalization and inpainting (OpenCV)

Two optional **C++** tools are built when CMake finds **OpenCV** with **core**, **imgcodecs**, **imgproc**, and **photo** (on Debian/Ubuntu: `libopencv-dev`). Binaries are written to **`dectris_data_consumer/bin/`** (see CMake output directory settings).

**Order in a simple pipeline:** run **`normalize_images`** first on raw or exported TIFF stacks, then **`inpaint_tiff`** on that output. Inpainting expects normalized float TIFFs (e.g. after flat-field correction), not raw detector frames.

**`normalize_images`** loads one **flat-field** TIFF and every `.tif` / `.tiff` in an input folder. For each frame it computes a transmission-style image: `object / (flat + epsilon)` (default `epsilon` is `1e-6` to avoid division by zero). Results are written as float TIFFs under the output directory (`--out`). Use **`--epsilon`** if you need a different floor on the denominator.

**`inpaint_tiff`** takes a **mask** TIFF (single channel): mask value **0** means a good pixel (unchanged); any **positive** mask value marks a bad pixel to inpaint. It supports folder batch mode (`--in` / `--out`), radius, NaN handling, algorithm (`telea` or `ns`), and optional noise on inpainted pixels (`--inpaint-noise-scale`). See **`--help`** in the tool for the full option list.

Examples (adjust paths to your tree; here **`example_dataset/`** holds flat field, originals, mask, and created **`normalized/`** and **`inpainted/`** folders):

```sh
./dectris_data_consumer/bin/normalize_images \
  --in example_dataset/original/ \
  --flat example_dataset/flatfield.tiff \
  --out example_dataset/normalized/

./dectris_data_consumer/bin/inpaint_tiff \
  --mask example_dataset/bad_pixel_mask.tiff \
  --in example_dataset/normalized/ \
  --out example_dataset/inpainted/ \
  --inpaint-noise-scale 1.0 --radius 3 --nan-fill 1.0 --algorithm ns
```

**Buffer caps (default 40 GiB each):** **`STREAM2_BUFFER_GB`** limits the **decoded** pixel stack used by `acquire_and_save_stream` and `DectrisStream2Receiver_linux` after receive. **`STREAM2_WIRE_BUFFER_GB`** limits the **as-received** buffer during streaming (if unset, falls back to `STREAM2_BUFFER_GB` / default). Tools that only buffer the wire (e.g. `stream2_buffer`, bifurcator) use the wire limit helper with the same env vars.

**Linux receiver:** optional **`STREAM2_NET_IFACE`** selects the network interface for binding when applicable (see program help / source).

### Bifurcator usage

```sh
# Receive from 192.168.1.100:31001, publish on 192.168.2.1:31002
./stream2_bifurcator 192.168.1.100 192.168.2.1 31002
```

Downstream clients should use ZMQ `PULL` like other Stream V2 tools. The bifurcator uses `PUSH` with timeouts; slow consumers may see send timeouts while data stays buffered locally.

On shutdown it can flush buffered images to TIFF; thread count uses **`STREAM2_TIFF_THREADS`** (default `10`, overridable in code paths that call the multi-threaded writer).

### Bifurcator tuning (e.g. ConnectX NICs)

```sh
STREAM2_RCVBUF_MB=512 \
STREAM2_SNDBUF_MB=512 \
STREAM2_BUSY_POLL_US=50 \
STREAM2_CPU_AFFINITY=0 \
STREAM2_IO_THREADS=4 \
STREAM2_REALTIME=1 \
./stream2_bifurcator 192.168.1.100 192.168.2.1 31002
```

| Variable | Meaning | Default |
|----------|---------|---------|
| `STREAM2_RCVBUF_MB` / `STREAM2_SNDBUF_MB` | ZMQ socket buffers (MB) | 256 |
| `STREAM2_BUSY_POLL_US` | Busy-poll timeout (µs); `0` = off | 0 |
| `STREAM2_CPU_AFFINITY` | Pin process to a CPU core | off |
| `STREAM2_IO_THREADS` | ZMQ I/O threads | 2 |
| `STREAM2_REALTIME` | Realtime priority (Linux; often needs root) | 0 |

**Host sysctl (Linux, as root)** — raise socket buffer limits when pushing high throughput:

```sh
sysctl -w net.core.rmem_max=536870912
sysctl -w net.core.wmem_max=536870912
sysctl -w net.core.rmem_default=536870912
sysctl -w net.core.wmem_default=536870912
```

Match IRQ affinity to your CPU/NIC layout if you pin the process (`/proc/interrupts`, `smp_affinity`). For dedicated NICs, consider stopping `irqbalance`.

*Tested on DGX systems with ConnectX-7 and on Xeon servers with ConnectX-6.*

### Build — Linux

Initialize submodules, then configure (system libzmq or bundled build):

```sh
git submodule update --init --recursive
mkdir build && cd build
cmake /path/to/birmenstorf/dectris_data_consumer -DCMAKE_BUILD_TYPE=Debug -DBUILD_LIBZMQ=YES
cmake --build .
./stream2_dump YOUR_DCU_HOST
```

Set **`BUILD_LIBZMQ=YES`** to fetch and build ZeroMQ from source; otherwise CMake expects an installed `libzmq`.

### Build — Windows

**OpenCV** (for `normalize_images` / `inpaint_tiff`): CMake must find **OpenCVConfig.cmake**. With the [official Windows pack](https://docs.opencv.org/4.x/d3/d52/tutorial_windows_install.html), that file is usually under `opencv\build` or `opencv\build\x64\vc17` (use the **vc*** folder that matches your Visual Studio). You can pass `-DOpenCV_DIR=...` on the configure line, or set `$env:OpenCV_DIR='...'` in the same PowerShell session (**`setx` does not affect the current window**). If you point **OpenCV_DIR** at the top **build** folder only, this project will also search **build/x64/vc*** and pick the newest match.

With **vcpkg**: install `opencv4[core,imgcodecs,imgproc,photo]:x64-windows` and pass `-DCMAKE_TOOLCHAIN_FILE=.../vcpkg/scripts/buildsystems/vcpkg.cmake` so `find_package(OpenCV)` works without **OpenCV_DIR**.

Open a **Visual Studio** developer shell (adjust paths), then:
```powershell
$vs="C:\Program Files\Microsoft Visual Studio\18\Community"
& "$vs\Common7\Tools\Launch-VsDevShell.ps1" -Arch amd64 -HostArch amd64  
cd birmenstorf\dectris_data_consumer
cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug -DBUILD_LIBZMQ=ON `
cmake --build .
```


[dectris-compression]: https://github.com/dectris/compression
[tinycbor]: https://github.com/intel/tinycbor
