#! /usr/bin/env python3
"""
Simple EIGER acquisition with stream2-style settings (monitor/filewriter off, stream on).

Mirror of: stream_v2/examples/eiger_client_demo/cpp/simple_acquisition_with_stream2.cpp
"""

from DEigerClient import DEigerClient

if __name__ == '__main__':

    # =============================================================================
    # USER INPUT
    # =============================================================================
    # Detector IP and port (DCU = detector control unit). Typical link-local: 169.254.254.1 /24
    DCU_IP = 'dev-si-e2dcu-06.dectris.local'
    DCU_PORT = 80
    force_initialization = False  # Force initialization even when already idle
    # Data acquisition
    thresholds = [15000]  # Energy thresholds [eV], 1 or 2 values, e.g. [10000, 15000]
    number_of_images = 1000
    number_of_triggers = 1  # Each trigger acquires number_of_images frames
    exposure_time = 1.0 / 1000.0  # Count time per image [s] (e.g. 1/fps)
    sleep_time = 0.0  # Extra delay per frame [s]; frame_time = exposure_time + sleep_time

    # =============================================================================
    # CONNECT TO DETECTOR
    # =============================================================================
    c = DEigerClient.DEigerClient(host=DCU_IP, port=DCU_PORT)

    # =============================================================================
    # INITIALIZE
    # =============================================================================
    print(f"Detector status:\t\t\t{c.detectorStatus('state')['value']}")
    # Initialize if forced or not idle
    if force_initialization or c.detectorStatus('state')['value'] != 'idle':
        print("Initializing detector...")
        c.sendDetectorCommand("initialize")

    # =============================================================================
    # CONFIGURATION
    # =============================================================================
    # Usual settings for polychromatic beam
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
    if len(thresholds) < 2:
        c.setDetectorConfig("threshold/2/mode", 'disabled')
    c.setDetectorConfig("count_time", exposure_time)
    c.setDetectorConfig('frame_time', exposure_time + sleep_time)
    c.setDetectorConfig("nimages", number_of_images)
    c.setDetectorConfig("ntrigger", number_of_triggers)

    # Useful status readout
    print(f"High voltage status:\t\t{c.detectorStatus('high_voltage/state')['value']}")
    print(f"Temperature:\t\t\t{c.detectorStatus('temperature')['value']:.1f} deg")
    print(f"Humidity:\t\t\t{c.detectorStatus('humidity')['value']:.1f} %")
    print(f'Count time= {c.detectorConfig("count_time")["value"]}')
    print(f'Frame time= {c.detectorConfig("frame_time")["value"]}')

    # =============================================================================
    # DATA ACQUISITION INTERFACES
    # =============================================================================
    c.setMonitorConfig("mode", "disabled")
    c.setFileWriterConfig('mode', "disabled")
    c.setStreamConfig('mode', "enabled")
    c.setStreamConfig('format', 'cbor')
    c.setStreamConfig('header_detail', 'all')

    # =============================================================================
    # RUN ACQUISITION
    # =============================================================================
    print("Acquiring data...")
    c.sendDetectorCommand('arm')
    # Software triggers; comment out if using external trigger
    for i in range(number_of_triggers):
        print(f"Triggering image {i+1}/{number_of_triggers}...")
        c.sendDetectorCommand('trigger')
    c.sendDetectorCommand("disarm")
