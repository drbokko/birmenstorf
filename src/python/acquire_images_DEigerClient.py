import json
from DEigerClient import DEigerClient
from datetime import datetime
from pathlib import Path
import os
import time
from hdf5_files.download_hdf5 import download_hdf5_save_images

# function to save the configuration in a json file
def save_config_to_json(config, save_path):
    config_serializable = {key: str(value) if isinstance(value, Path) else value for key, value in config.items()}
    config_file = Path(save_path) / "detector_settings.json"
    with open(config_file, "w") as f:
        json.dump(config_serializable, f, indent=4)
    print(f"Configuration saved to {config_file}")


if __name__ == '__main__':
            
    ################
    ## USER INPUT ##
    ################
    configuration = {
        # Detector IP and Initialization
        "DCU_IP": 'dev-si-e2dcu-06.dectris.local',  # IP of the DCU (detector control unit). Static IP: 169.254.254.1 Netmask: 255.255.255.0
        "force_initialization": False,  # Force initialization if needed

        # File & Naming Settings
        "working_directory": "C:/Measurements/temp", # base directory
        "measurement_name": f'test',
        "acquisition_time": datetime.now().strftime('%Y%m%d_%H%M%S'),

        # Data acquisition parameters
        "thresholds": [8000],  # Energy thresholds [eV], insertfrom 1 to 2 values. e.g. [10000, 15000] for 2 thresholds
        "number_of_images":1000,  # Number of images to capture
        "number_of_triggers":2,
        "exposure_time": 1.0/100.0,  # Exposure time per image [s]. 1/fps
        "sleep_time": 0.0,  # Delay between frames [s]
        
        # Data Acquisition Interfaces
        "monitor": "disabled", # slow, only for framerates below 10 fps
        "filewriter2": "enabled", # save imageso on the DCU on h5 format. 
        "stream": "enabled", # stream images directly to the client. For high performance it requires a fast connection between DCU and client
        "save_raw": False,  # Save raw data if using filewriter2, otherwise images are saved as tiff
    }
    # Construct full output directory path and save config
    configuration["full_path"] = Path(configuration["working_directory"], f"{configuration['acquisition_time']}_{configuration['measurement_name']}")
    os.makedirs(configuration["full_path"], exist_ok=True)  # Ensure directory exists
    save_config_to_json(configuration, configuration["full_path"])

    #######################
    # Connect to Detector #
    #######################
    c = DEigerClient.DEigerClient(host=configuration["DCU_IP"])

    ##############
    # Initialize #
    ##############
    print(f"Detector status:\t\t\t{c.detectorStatus('state')['value']}")
    # initialize the detector if needed or enforced
    if configuration["force_initialization"] or c.detectorStatus('state')['value'] != 'idle':
        print("Initializing detector...")
        c.sendDetectorCommand("initialize")

    # ##############
    # #  HV reset  #
    # ##############
    # for i in range(3):
    #     while c.detectorStatus('high_voltage/state')['value'] != 'READY':
    #         print(f"High voltage state {c.detectorStatus('high_voltage/state')['value']}, waiting...")
    #         time.sleep(5)
    #     print(f"High voltage status:\t\t{c.detectorStatus('high_voltage/state')['value']}")
    #     print(f"Resetting high voltage... Attempt {i+1}/3")
    #     c.sendDetectorCommand("hv_reset")

    ##################
    # Configuration #
    ##################
    # usual settings for polychromatic beam
    c.setDetectorConfig('auto_summation', False)
    c.setDetectorConfig("countrate_correction_applied", False)
    c.setDetectorConfig('retrigger', False)
    c.setDetectorConfig('counting_mode', 'normal')
    c.setDetectorConfig('trigger_mode', 'ints')
    c.setDetectorConfig("flatfield_correction_applied", False)
    c.setDetectorConfig('virtual_pixel_correction_applied', True)
    c.setDetectorConfig('mask_to_zero', True) #pixels marked in the pixel_mask will be set to zero
    c.setDetectorConfig("test_image_mode", "") # "", value, cal_pulse, mcb_id
    c.setDetectorConfig("test_image_value", 100) # valid only if "value" is selected as test_image_mode. 

    # acquisitions parameters
    for i, th in enumerate(configuration["thresholds"], start=1):
        c.setDetectorConfig(f"threshold/{i}/mode", 'enabled')
        c.setDetectorConfig(f"threshold/{i}/energy", th)
    if len(configuration["thresholds"]) < 2:
        c.setDetectorConfig("threshold/2/mode", 'disabled')
    c.setDetectorConfig("count_time", configuration["exposure_time"])
    c.setDetectorConfig('frame_time', configuration["exposure_time"] + configuration["sleep_time"])
    c.setDetectorConfig("nimages", configuration["number_of_images"])
    c.setDetectorConfig("ntrigger", configuration["number_of_triggers"])  

    #print some useful information 
    print(f"High voltage status:\t\t{c.detectorStatus('high_voltage/state')['value']}")
    print(f"Temperature:\t\t\t{c.detectorStatus('temperature')['value']:.1f} deg")
    print(f"Humidity:\t\t\t{c.detectorStatus('humidity')['value']:.1f} %")
    print(f'Count time= {c.detectorConfig("count_time")["value"]}')
    print(f'Frame time= {c.detectorConfig("frame_time")["value"]}')

    ###############################
    # Data Acquisition Interfaces #
    ###############################
    # Monitor
    c.setMonitorConfig("mode", configuration["monitor"])
    # Filewriter
    c.setFileWriterConfig('mode', configuration["filewriter2"])
    if configuration["filewriter2"] == "enabled":
        c.setFileWriterConfig('name_pattern', f"{configuration['acquisition_time']}_{configuration['measurement_name']}")
        c.setFileWriterConfig('compression_enabled', False)
        c.setFileWriterConfig('format', 'hdf5 nexus v2024.2 nxmx')
    # Stream
    c.setStreamConfig('mode', configuration["stream"])
    c.setStreamConfig('format', 'cbor')
    c.setStreamConfig('header_detail', 'all')

    ###################
    # Run Acquisition #
    ###################
    print("Acquiring data...")
    c.sendDetectorCommand('arm')
    for i in range(configuration["number_of_triggers"]):
        print(f"Triggering image {i+1}/{configuration['number_of_triggers']}...")   
        c.sendDetectorCommand('trigger')
    c.sendDetectorCommand("disarm")  

    #######################
    # Retrieve the Images #
    #######################
    print("Retrieving images...")
    start = time.time()

    if configuration["monitor"] == "enabled":
        for trigger_id in range( c.detectorConfig("ntrigger")["value"]):
            for image_id in range(c.detectorConfig("nimages")['value']):
                for th in range(len(configuration["thresholds"])):
                    c.monitorSave(param="next", path=Path(configuration["full_path"], f"img_th{th+1}_tr{trigger_id}_{image_id:05d}.tif"))
        c.sendMonitorCommand('clear') 

    if configuration["filewriter2"] == "enabled":
        download_hdf5_save_images(
            dcu_host=configuration["DCU_IP"],
            hdf5_file_root_name=f"{configuration['acquisition_time']}_{configuration['measurement_name']}",
            nimages_per_file=c.fileWriterConfig('nimages_per_file') ["value"],
            n_images=configuration["number_of_images"],
            save_path=configuration["full_path"],
            save_raw=configuration["save_raw"]
        )
        c.sendFileWriterCommand('clear')

    print(f"\tImages retrieved in  {time.time() - start:.2f}s")
