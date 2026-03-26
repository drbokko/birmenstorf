#! /usr/bin/env python3
"""
EIGER demo: 1000 images in one batch over the stream v2 interface (CBOR), with a single
software (internal) trigger and one energy threshold.

Workflow: connect to the DCU; call initialize if state is not idle; configure detector
(see CONFIGURATION IMPORTANT: corrections, counting_mode, thresholds, no photon_energy;
monitor/filewriter off, stream on); read high voltage state, temperature, and humidity; enable stream
(monitor and filewriter off), format CBOR; arm; send trigger(s). The detector must be
armed before it accepts trigger.

This script does not implement the stream consumer: run a receiver that connects to the
detector’s stream v2 endpoint in a separate process. The trigger command may also be
issued from another process if you coordinate arm/disarm and timing yourself.

Disclaimer: this sequence is a minimal demo (here, one trigger acquires nimages frames).
For continuous acquisition, increase ntrigger (and configure the detector accordingly)
rather than relying on a single trigger batch.

C++ equivalent split across cpp/connect_and_configure_and_arm_detector.cpp,
cpp/send_software_trigger.cpp, and cpp/wait_idle_and_disarm_detector.cpp.
"""

import time

from DEigerClient import DEigerClient

if __name__ == '__main__':

    # =============================================================================
    # USER INPUT
    # =============================================================================
    # Detector IP and port (DCU = detector control unit). Typical link-local: 169.254.254.1 /24
    DCU_IP = '169.254.254.1'
    DCU_PORT = 80
    force_initialization = False  # Force initialization even when already idle
    # Data acquisition (software trigger: trigger command from host; ntrigger=1 → one batch)
    thresholds = [15000]  # Single threshold [eV]; second threshold disabled below
    number_of_images = 1000
    number_of_triggers = 1  # Each trigger acquires number_of_images frames
    exposure_time = 1.0 / 1000.0  # Count time per image [s] (e.g. 1/fps)
    sleep_time = 0.0  # Extra delay per frame [s]; frame_time = exposure_time + sleep_time

    # =============================================================================
    # CONNECT TO DETECTOR
    # =============================================================================
    c = DEigerClient.DEigerClient(host=DCU_IP, port=DCU_PORT, verbose=True)

    # =============================================================================
    # INITIALIZE
    # =============================================================================
    print(f"Detector status:\t\t\t{c.detectorStatus('state')['value']}")
    # Initialize if forced or not idle
    if force_initialization or c.detectorStatus('state')['value'] != 'idle':
        print("Initializing detector...")
        c.sendDetectorCommand("initialize")

    # =============================================================================
    # CONFIGURATION  (IMPORTANT)
    # =============================================================================
    # Detector (Simplon-style layout for this stream demo):
    #   Disable: countrate_correction_applied, retrigger, flatfield_correction_applied,
    #            auto_summation.
    #   counting_mode is a string parameter (not a bool): set explicitly as required;
    #   this demo uses 'normal'.
    #   Enable: virtual_pixel_correction_applied, mask_to_zero.
    #   Then: threshold mode/energy, count_time, frame_time, nimages, trigger_mode, ntrigger.
    #   Do NOT set photon_energy: it can overwrite threshold-related settings.
    #   trigger_mode is a string: "ints" for internal trigger, "exts" for external trigger.
    #   Default is "ints" but safer to set explicitly. 1 trigger acquires nimages frames.
    # Data path:
    #   Disable monitor; disable filewriter; enable stream (CBOR etc. below).
    # Usual settings for polychromatic beam
    c.setDetectorConfig("countrate_correction_applied", False)
    c.setDetectorConfig('retrigger', False)
    c.setDetectorConfig('counting_mode', 'normal')  # see IMPORTANT: mode string, not "disabled"
    c.setDetectorConfig("flatfield_correction_applied", False)
    c.setDetectorConfig('virtual_pixel_correction_applied', True)
    c.setDetectorConfig('mask_to_zero', True)
    c.setDetectorConfig('auto_summation', False)

    # Thresholds and timing
    for i, th in enumerate(thresholds, start=1):
        c.setDetectorConfig(f"threshold/{i}/mode", 'enabled')
        c.setDetectorConfig(f"threshold/{i}/energy", th)
    if len(thresholds) < 2:
        c.setDetectorConfig("threshold/2/mode", 'disabled')
    c.setDetectorConfig("count_time", exposure_time)
    c.setDetectorConfig('frame_time', exposure_time + sleep_time)
    c.setDetectorConfig("nimages", number_of_images)
    c.setDetectorConfig("trigger_mode", "ints")  # "ints" internal, "exts" external
    c.setDetectorConfig("ntrigger", number_of_triggers)

    # Useful status readout
    print(f"High voltage status: {c.detectorStatus('high_voltage/state')['value']}")
    print(f"Temperature: {c.detectorStatus('temperature')['value']:.1f} deg")
    print(f"Humidity: {c.detectorStatus('humidity')['value']:.1f} %")
    print(f'Count time= {c.detectorConfig("count_time")["value"]}')
    print(f'Frame time= {c.detectorConfig("frame_time")["value"]}')

    # =============================================================================
    # DATA ACQUISITION INTERFACES (stream v2: CBOR on; consumer runs elsewhere)
    # =============================================================================
    c.setMonitorConfig("mode", "disabled")
    c.setFileWriterConfig('mode', "disabled")
    c.setStreamConfig('mode', "enabled")
    c.setStreamConfig('format', 'cbor')
    c.setStreamConfig('header_detail', 'all')

    # =============================================================================
    # RUN ACQUISITION (arm required before trigger; trigger may be moved to another process)
    # =============================================================================
    print("Acquiring data...")
    c.sendDetectorCommand('arm')
    # Software trigger from this process; omit this loop if another process sends trigger
    for i in range(number_of_triggers):
        print(f"Triggering image {i+1}/{number_of_triggers}...")
        c.sendDetectorCommand('trigger')
    while c.detectorStatus('state')['value'] != 'idle':
        time.sleep(0.1)
    c.sendDetectorCommand("disarm")
