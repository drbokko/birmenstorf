# DECTRIS Stream V2 — Consumer and producer 


## Producer and consumer

The **DCU** is the **producer**: when **armed**, it publishes frames on Stream V2 at `tcp://<dcu>:31001`. Your workstation runs **consumers** that **PULL** that stream, decode CBOR, and save or process images. REST programs such as [`connect_and_configure_and_arm_detector`](../dectris_detector_control/cpp/connect_and_configure_and_arm_detector.cpp) and [`software_trigger_detector`](../dectris_detector_control/cpp/software_trigger_detector.cpp) control acquisition; tools **here**—[`acquire_and_save_stream`](acquire_and_save_stream.c), [`DectrisStream2Receiver_linux`](DectrisStream2Receiver_linux.c), etc.—receive the stream. **Start the stream receiver before frames are emitted** whenever you must not lose data.

## Sample workflow (tools in this repo)

1. **Workstation and DCU** — Power on chiller and detector, confirm cooling, and network connections.
2. **Configure and arm** — Build and run the producer tool (see [`dectris_detector_control`](../dectris_detector_control/) Makefile):

   ```sh
   cd ../dectris_detector_control && make
   ./connect_and_configure_and_arm_detector <dcu_ip_address>
   ```

   Source: [`connect_and_configure_and_arm_detector.cpp`](../dectris_detector_control/cpp/connect_and_configure_and_arm_detector.cpp). Set **`nimages`** / **`ntrigger`** in that source to match your plan (e.g. **1000** frames per trigger and enough **`ntrigger`** budget for the triggers you will send).

3. **Generate a non-normalized flatfield** — Average **1000** stream frames into one **float32** TIFF (mean in detector units). Configure step 2 for **1000** **`nimages`** per trigger (and enough **`ntrigger`**). On the **receiver**, start the stream consumer **before** you trigger.

   From your **`dectris_data_consumer`** CMake build directory (e.g. `build/`, with `acquire_and_save_stream` on `PATH` or `./bin/acquire_and_save_stream`):

   ```sh
   > ./acquire_and_save_stream <dcu_ip_address> -n 1000 --generate-flatfield-only --flatfield-file flatfield.tiff
   ```

   Then trigger acquisition—for example **one** software trigger if each trigger emits 1000 frames—from **`dectris_detector_control`** (after `make`):

   ```sh
   > ./software_trigger_detector <dcu_ip_address> -n 1
   ```

   Code: [`acquire_and_save_stream.c`](acquire_and_save_stream.c), [`software_trigger_detector.cpp`](../dectris_detector_control/cpp/software_trigger_detector.cpp). The trigger **`-n`** must stay consistent with **`ntrigger`** from step 2. When acquisition finishes, **`flatfield.tiff`** is at the **`--flatfield-file`** path.

4. **Routine streaming** — Start your stream receiver (e.g. [`DectrisStream2Receiver_linux`](DectrisStream2Receiver_linux.c)), trigger as needed, and use **`wait_idle_and_disarm_detector`** when finishing the acquisition sequence (see [`dectris_detector_control`](../dectris_detector_control/) README).

---
