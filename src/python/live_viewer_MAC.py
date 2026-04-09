import queue
import signal
import threading
import time

# --- Detector control imports ---
from DEigerClient import DEigerClient

# --- Stream receiver imports ---
import cbor2
from dectris.compression import decompress
import numpy as np

############################################################
# Configuration (no saving; live view only)
############################################################
configuration = {
    # Detector IP and Initialization
    "DCU_IP": 'dev-si-e2dcu-06.dectris.local',
    "force_initialization": False,

    # Data acquisition parameters
    "thresholds": [10000],  # Energy thresholds [eV], 1..4 values supported
    "number_of_images": 1,       # Number of images to capture
    "number_of_triggers": 1000,       # Number of trigger to send
    "exposure_time": 1.0/10.0,          # Exposure time per image [s]
    "sleep_time": 0.5,             # Delay between frames [s]

    # Live viewer performance knobs
    "live_every": 1,               # Show every Nth frame in live view
    "stats_subsample": 8,          # Subsample factor for median/contrast
    "max_fps": 20.0,               # Cap redraw rate (frames/sec)

    # Geometry: imshow uses (rows, cols) = (height, width). EIGER2 1M-W is 2068×512 px (wide).
    # If the stream sends (width, height) order, we transpose so the window matches the detector.
    "live_image_shape_hw": (512, 2068),
}



############################################################
# CBOR/DECTRIS helpers
############################################################

def decode_multi_dim_array(tag, column_major):
    dimensions, contents = tag.value
    if isinstance(contents, list):
        array = np.empty((len(contents),), dtype=object)
        array[:] = contents
    elif isinstance(contents, (np.ndarray, np.generic)):
        array = contents
    else:
        raise cbor2.CBORDecodeValueError("expected array or typed array")
    return array.reshape(dimensions, order="F" if column_major else "C")


def decode_typed_array(tag, dtype):
    if not isinstance(tag.value, bytes):
        raise cbor2.CBORDecodeValueError("expected byte string in typed array")
    return np.frombuffer(tag.value, dtype=dtype)


def decode_dectris_compression(tag):
    algorithm, elem_size, encoded = tag.value
    return decompress(encoded, algorithm, elem_size=elem_size)


tag_decoders = {
    40: lambda tag: decode_multi_dim_array(tag, column_major=False),
    64: lambda tag: decode_typed_array(tag, dtype="u1"),
    65: lambda tag: decode_typed_array(tag, dtype=">u2"),
    66: lambda tag: decode_typed_array(tag, dtype=">u4"),
    67: lambda tag: decode_typed_array(tag, dtype=">u8"),
    68: lambda tag: decode_typed_array(tag, dtype="u1"),
    69: lambda tag: decode_typed_array(tag, dtype="<u2"),
    70: lambda tag: decode_typed_array(tag, dtype="<u4"),
    71: lambda tag: decode_typed_array(tag, dtype="<u8"),
    72: lambda tag: decode_typed_array(tag, dtype="i1"),
    73: lambda tag: decode_typed_array(tag, dtype=">i2"),
    74: lambda tag: decode_typed_array(tag, dtype=">i4"),
    75: lambda tag: decode_typed_array(tag, dtype=">i8"),
    77: lambda tag: decode_typed_array(tag, dtype="<i2"),
    78: lambda tag: decode_typed_array(tag, dtype="<i4"),
    79: lambda tag: decode_typed_array(tag, dtype="<i8"),
    80: lambda tag: decode_typed_array(tag, dtype=">f2"),
    81: lambda tag: decode_typed_array(tag, dtype=">f4"),
    82: lambda tag: decode_typed_array(tag, dtype=">f8"),
    83: lambda tag: decode_typed_array(tag, dtype=">f16"),
    84: lambda tag: decode_typed_array(tag, dtype="<f2"),
    85: lambda tag: decode_typed_array(tag, dtype="<f4"),
    86: lambda tag: decode_typed_array(tag, dtype="<f8"),
    87: lambda tag: decode_typed_array(tag, dtype="<f16"),
    1040: lambda tag: decode_multi_dim_array(tag, column_major=True),
    56500: lambda tag: decode_dectris_compression(tag),
}


def tag_hook(decoder, tag):
    tag_decoder = tag_decoders.get(tag.tag)
    return tag_decoder(tag) if tag_decoder else tag


def prepare_live_image(data: np.ndarray, shape_hw: tuple[int, int] | None) -> np.ndarray:
    """Return array shaped (height, width) for imshow; transpose if stream order is swapped."""
    arr = np.asarray(data)
    if shape_hw is None:
        return arr
    h, w = shape_hw
    if arr.shape == (h, w):
        return arr
    if arr.shape == (w, h):
        return arr.T
    return arr


############################################################
# Stream Receiver (thread) — ZMQ only; GUI runs on main thread (macOS requirement)
############################################################

class StreamReceiver(threading.Thread):
    def __init__(
        self,
        ip: str,
        live_queue: "queue.Queue",
        live_every: int = 5,
    ):
        super().__init__(daemon=True)
        self.ip = ip
        self.endpoint = f"tcp://{ip}:31001"
        self.live_queue = live_queue
        self.live_every = max(1, int(live_every))
        self.stop_event = threading.Event()
        self.finished_event = threading.Event()

        self.image_num = 0
        self.channel = 'threshold_1'  # plot only first threshold

    def stop(self):
        self.stop_event.set()

    def _put_live(self, item):
        try:
            self.live_queue.put_nowait(item)
        except queue.Full:
            pass  # drop: main thread is behind; keep stream responsive

    def run(self):
        import zmq

        context = zmq.Context()
        socket = context.socket(zmq.PULL)
        socket.setsockopt(zmq.RCVTIMEO, 1000)   # check stop_event regularly
        socket.setsockopt(zmq.RCVHWM, 10)       # limit backlog so UI stays responsive

        try:
            socket.connect(self.endpoint)
            print(f"[STREAM] Connected PULL {self.endpoint}")

            start_time = None

            while not self.stop_event.is_set():
                try:
                    raw = socket.recv()
                except zmq.Again:
                    continue  # timeout: loop and check stop_event

                message = cbor2.loads(raw, tag_hook=tag_hook)
                mtype = message.get('type')

                if mtype == 'start':
                    start_time = time.time()
                    print(f"[STREAM] start")
                    self.image_num = 0
                    th = message.get('threshold_energy', {})
                    thresholds = [th.get('threshold_1')]
                    self._put_live(('start', message.get('count_time'), thresholds))

                elif mtype == 'image':
                    self.image_num += 1
                    img_id = int(message.get('image_id', self.image_num))
                    print(f"[STREAM] {img_id} image received")
                    draw_this = (img_id % self.live_every == 0)

                    data = message['data'].get(self.channel)
                    if data is None:
                        first_key = next(iter(message['data']))
                        data = message['data'][first_key]
                    data = np.asarray(data)

                    if draw_this:
                        self._put_live(('image', img_id, data))

                elif mtype == 'end':
                    elapsed = (time.time() - start_time) if start_time else 0
                    print(f"[STREAM] {self.image_num} images in {elapsed:.1f}s (live view only)")
                    self._put_live(('end', self.image_num, elapsed))
                    break

        except Exception as e:
            print(f"[STREAM] Error: {e}")
            self._put_live(('error', str(e)))
        finally:
            try:
                socket.close(0)
            except Exception:
                pass
            context.term()
            self.finished_event.set()
            print("[STREAM] Receiver stopped")


def setup_live_view(shape_hw: tuple[int, int] | None = None, fig_height_in: float = 3.0):
    """Create the live window on the **main** thread (required on macOS).

    Figure aspect follows the detector so a wide module is not stretched into a square axes.
    """
    import matplotlib.pyplot as plt

    plt.ion()
    if shape_hw is not None:
        h, w = shape_hw
        aspect_wh = w / h
        fig_w = fig_height_in * aspect_wh
        figsize = (fig_w, fig_height_in)
        placeholder = np.zeros((h, w), dtype=np.uint16)
    else:
        figsize = (12, 5)
        placeholder = np.zeros((10, 10), dtype=np.uint16)

    fig, ax = plt.subplots(1, 1, figsize=figsize)
    ax.set_title("Threshold 1")
    ax.set_xticks([])
    ax.set_yticks([])
    im = ax.imshow(
        placeholder,
        cmap="gray",
        vmin=0,
        vmax=1,
        animated=False,
        aspect="equal",
        interpolation="nearest",
    )
    ax.set_aspect("equal")
    return fig, ax, im


############################################################
# Main
############################################################

if __name__ == '__main__':
    # Connect to Detector
    c = DEigerClient.DEigerClient(host=configuration["DCU_IP"])
    print(f"Detector status:\t\t\t{c.detectorStatus('state')['value']}")

    if configuration["force_initialization"] or c.detectorStatus('state')['value'] != 'idle':
        print("Initializing detector...")
        c.sendDetectorCommand("initialize")

    print(f"Detector high_voltage status:\t\t{c.detectorStatus('high_voltage/state')['value']}")
    print(f"Detector temperature:\t\t\t{c.detectorStatus('temperature')['value']:.1f} deg")
    print(f"Detector humidity:\t\t\t{c.detectorStatus('humidity')['value']:.1f} %")

    # Configure Detector
    c.setDetectorConfig("countrate_correction_applied", False)
    c.setDetectorConfig('retrigger', False)
    c.setDetectorConfig('counting_mode', 'normal')
    c.setDetectorConfig("flatfield_correction_applied", False)
    c.setDetectorConfig('virtual_pixel_correction_applied', True)
    c.setDetectorConfig('mask_to_zero', True)  # pixels marked in the pixel_mask will be set to zero
    c.setDetectorConfig("test_image_mode", "")
    # c.setDetectorConfig("test_image_value", 100)

    c.setDetectorConfig("photon_energy", 10000)  # example value for monochromatic

    # only one threshold (device has 2 thresholds total; we enable only the first and disable the second)
    c.setDetectorConfig("threshold/1/mode", 'enabled')
    c.setDetectorConfig("threshold/1/energy", configuration["thresholds"][0])
    c.setDetectorConfig("threshold/2/mode", 'disabled')

    # Set Acquisition Parameters
    c.setDetectorConfig("count_time", configuration["exposure_time"]) 
    c.setDetectorConfig('frame_time', configuration["exposure_time"] + configuration["sleep_time"]) 
    c.setDetectorConfig("nimages", configuration["number_of_images"]) 
    c.setDetectorConfig("ntrigger", configuration["number_of_triggers"])  
    c.setDetectorConfig('auto_summation', False)

    print(f'count_time= {c.detectorConfig("count_time")["value"]}')
    print(f'frame_time= {c.detectorConfig("frame_time")["value"]}')

    # Configure Interfaces (saving disabled)
    c.setMonitorConfig('mode', 'disabled')  # monitor explicitly OFF  
    c.setFileWriterConfig('mode', 'disabled')              # ensure filewriter is OFF
    c.setStreamConfig('mode', 'enabled') 
    c.setStreamConfig('format', 'cbor') 
    c.setStreamConfig('header_detail', 'all') 

    import matplotlib.pyplot as plt

    # GUI must run on the main thread on macOS; ZMQ + acquisition run in worker threads.
    live_queue: queue.Queue = queue.Queue(maxsize=32)
    shape_hw = configuration.get("live_image_shape_hw")
    fig, ax, im = setup_live_view(shape_hw=shape_hw)

    stream_thread = StreamReceiver(
        ip=configuration["DCU_IP"],
        live_queue=live_queue,
        live_every=configuration["live_every"],
    )

    acq_done = threading.Event()

    def run_acquisition():
        try:
            print("Acquiring data...")
            print("Arming")
            c.sendDetectorCommand('arm')
            for trigger in range(configuration['number_of_triggers']):
                c.sendDetectorCommand('trigger')
            print("disarming")
            c.sendDetectorCommand('disarm')
        except Exception as e:
            print(f"[ACQ] Error: {e}")
        finally:
            acq_done.set()

    acq_thread = threading.Thread(target=run_acquisition, daemon=True)
    shutdown = threading.Event()

    def handle_sigint(sig, frame):
        print("\n[MAIN] Caught interrupt. Stopping...")
        shutdown.set()
        stream_thread.stop()

    signal.signal(signal.SIGINT, handle_sigint)

    stream_thread.start()
    acq_thread.start()

    stats_subsample = max(1, int(configuration["stats_subsample"]))
    max_dt = 1.0 / max(1.0, float(configuration["max_fps"]))
    last_draw = 0.0
    thresholds: list = []

    try:
        while not shutdown.is_set():
            if acq_done.is_set() and stream_thread.finished_event.is_set():
                break
            try:
                item = live_queue.get(timeout=0.05)
            except queue.Empty:
                plt.pause(0.001)
                continue

            kind = item[0]
            if kind == 'start':
                _, _count_time, thresholds = item
                plt.pause(0.001)
            elif kind == 'error':
                print(f"[LIVE] {item[1]}")
                plt.pause(0.001)
            elif kind == 'end':
                plt.pause(0.001)  # already logged in StreamReceiver.run
            elif kind == 'image':
                img_id, data = int(item[1]), item[2]
                data = prepare_live_image(data, shape_hw)
                ss = stats_subsample
                sample = data[::ss, ::ss]
                median_counts = float(np.median(sample))

                if im.get_array().shape != data.shape:
                    im.set_data(np.zeros_like(data))
                im.set_data(data)
                lo = median_counts * 0.8
                hi = max(lo + 1.0, median_counts * 1.2)
                im.set_clim(lo, hi)

                now = time.time()
                if now - last_draw >= max_dt:
                    if median_counts <= 10e3:
                        ax.set_title(f"Image {img_id} - {median_counts:.0f} cps (th={thresholds} eV)")
                    elif median_counts <= 10e6:
                        ax.set_title(f"Image {img_id} - {median_counts/1e3:.1f} kcps (th={thresholds} eV)")
                    else:
                        ax.set_title(f"Image {img_id} - {median_counts/1e6:.1f} Mcps (th={thresholds} eV)")
                    fig.canvas.draw_idle()
                    fig.canvas.flush_events()
                    last_draw = now
                plt.pause(0.001)
    finally:
        print("Stopping stream receiver...")
        stream_thread.stop()
        stream_thread.finished_event.wait(timeout=5.0)
        try:
            c.sendDetectorCommand('disarm')
        except Exception:
            pass

    print("All done.")

