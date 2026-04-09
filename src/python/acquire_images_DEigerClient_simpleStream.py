from DEigerClient import DEigerClient

if __name__ == '__main__':
            
    ################
    ## USER INPUT ##
    ################
    # Detector IP and Initialization
    DCU_IP= '169.254.254.1',            # IP of the DCU (detector control unit). Static IP: 169.254.254.1 Netmask: 255.255.255.0
    force_initialization= False,        # Force initialization if needed
    # Data acquisition parameters
    thresholds= [15000],                # Energy thresholds [eV], insertfrom 1 to 2 favuels. e.g. [10000, 15000] for 2 thresholds
    number_of_images=1000,              # Number of images to capture
    number_of_triggers=1,               # For every trigger, the specified number of images will be acquired. 
    exposure_time= 1.0/1000.0,          # Exposure time per image [s]. 1/fps
    sleep_time= 0.0,                    # Delay between images [s]
   
    #######################
    # Connect to Detector #
    #######################
    c = DEigerClient.DEigerClient(host=DCU_IP)

    ##############
    # Initialize #
    ##############
    print(f"Detector status:\t\t\t{c.detectorStatus('state')['value']}")
    # initialize the detector if needed or enforced
    if force_initialization or c.detectorStatus('state')['value'] != 'idle':
        print("Initializing detector...")
        c.sendDetectorCommand("initialize")

    ##################
    # Configuration #
    ##################
    # usual settings for polychromatic beam
    c.setDetectorConfig("countrate_correction_applied", False)
    c.setDetectorConfig('retrigger', False)
    c.setDetectorConfig('counting_mode', 'normal')
    c.setDetectorConfig('auto_summation', False)
    c.setDetectorConfig("flatfield_correction_applied", False)
    c.setDetectorConfig('virtual_pixel_correction_applied', True)
    c.setDetectorConfig('mask_to_zero', True) 
    
    # acquisitions parameters
    for i, th in enumerate(thresholds, start=1):
        c.setDetectorConfig(f"threshold/{i}/mode", 'enabled')
        c.setDetectorConfig(f"threshold/{i}/energy", th)
    if len(thresholds) < 2:
        c.setDetectorConfig("threshold/2/mode", 'disabled')
    c.setDetectorConfig("count_time", exposure_time)
    c.setDetectorConfig('frame_time', exposure_time + sleep_time)
    c.setDetectorConfig("nimages", number_of_images)
    c.setDetectorConfig("ntrigger", number_of_triggers)  

    #print some useful information 
    print(f"High voltage status:\t\t{c.detectorStatus('high_voltage/state')['value']}")
    print(f"Temperature:\t\t\t{c.detectorStatus('temperature')['value']:.1f} deg")
    print(f"Humidity:\t\t\t{c.detectorStatus('humidity')['value']:.1f} %")
    print(f'Count time= {c.detectorConfig("count_time")["value"]}')
    print(f'Frame time= {c.detectorConfig("frame_time")["value"]}')

    ###############################
    # Data Acquisition Interfaces #
    ###############################
    c.setMonitorConfig("mode", "disabled")
    c.setFileWriterConfig('mode', "disabled")
    c.setStreamConfig('mode', "enabled")
    c.setStreamConfig('format', 'cbor')
    c.setStreamConfig('header_detail', 'all')

    ###################
    # Run Acquisition #
    ###################
    print("Acquiring data...")
    c.sendDetectorCommand('arm')
    for i in range(number_of_triggers):                             #send software trigger, if external trigger is used, comment these lines
        print(f"Triggering image {i+1}/{number_of_triggers}...")   
       
        c.sendDetectorCommand('trigger') 
    
    c.sendDetectorCommand("disarm")  
