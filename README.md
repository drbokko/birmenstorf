# DECTRIS Stream V2 — Consumer and producer 


## Producer and consumer

The **DCU** is the **producer**: when **armed**, it publishes frames on Stream V2 at `tcp://<dcu>:31001`. Your workstation runs **consumers** that **PULL** that stream, decode CBOR, and save or process images. REST programs such as [`connect_and_configure_and_arm_detector`](dectris_detector_control/cpp/connect_and_configure_and_arm_detector.cpp) and [`send_software_trigger`](dectris_detector_control/cpp/send_software_trigger.cpp) control acquisition; tools **here**—[`acquire_and_save_stream`](dectris_data_consumer/acquire_and_save_stream.c), [`DectrisStream2Receiver_linux`](dectris_data_consumer/DectrisStream2Receiver_linux.c), etc.—receive the stream. **Start the stream receiver before frames are emitted** whenever you must not lose data.

## Sample workflow (tools in this repo)

1. **Workstation and DCU** — Power on chiller and detector, confirm cooling, and network connections.
2. **Configure and arm** — Build and run the producer tool (see [`dectris_detector_control`](dectris_detector_control/) Makefile):

   ```sh
   cd dectris_detector_control && make
   ./connect_and_configure_and_arm_detector <dcu_ip_address>
   ```

   Source / CLI: [`connect_and_configure_and_arm_detector.cpp`](dectris_detector_control/cpp/connect_and_configure_and_arm_detector.cpp). Use **`--nimages`** and **`--ntrigger`** (defaults **1000** / **100000**) to match your plan (e.g. **1000** frames per trigger and enough **`ntrigger`** for the bursts you will send).

3. **Generate a non-normalized flatfield** — Average **1000** stream frames into one **float32** TIFF (mean in detector units). Configure step 2 for **1000** **`nimages`** per trigger (and enough **`ntrigger`**). On the **receiver**, start the stream consumer **before** you trigger.

   From your **`dectris_data_consumer`** CMake build directory (e.g. `build/`, with `acquire_and_save_stream` on `PATH` or `./bin/acquire_and_save_stream`):

   ```sh
   > ./acquire_and_save_stream <dcu_ip_address> --nimages 1000 --generate-flatfield-only --flatfield-file flatfield.tiff
   ```

   Then trigger acquisition—for example **one** software trigger if each trigger emits 1000 frames—from **`dectris_detector_control`** (after `make`):

   ```sh
   > ./send_software_trigger <dcu_ip_address> --ntrigger 1
   ```

   Code: [`acquire_and_save_stream.c`](dectris_data_consumer/acquire_and_save_stream.c), [`send_software_trigger.cpp`](dectris_detector_control/cpp/send_software_trigger.cpp). **`--ntrigger`** must stay consistent with detector **`ntrigger`** from step 2; **`--nimages`** on the consumer should match frames per trigger when you need an exact frame count. When acquisition finishes, **`flatfield.tiff`** is at the **`--flatfield-file`** path.

4. **Routine streaming** — Start your stream receiver (e.g. [`DectrisStream2Receiver_linux`](dectris_data_consumer/DectrisStream2Receiver_linux.c)), trigger as needed, and use **`wait_idle_and_disarm_detector`** when finishing the acquisition sequence after idle. To **stop acquisition immediately** without waiting for idle, use **`disarm_detector`** (see [`dectris_detector_control`](dectris_detector_control/) README).

---
