#!/usr/bin/env python3
"""
Minimal script to get flatfield from EIGER detector and save as .npy file
Handles darray format with base64 encoding
"""

import numpy as np
import logging
from pathlib import Path
from DEigerClient import DEigerClient
from get_and_set_flatfield import decode_darray, configure_thresholds
import tifffile

logging.basicConfig(level=logging.INFO, format='%(levelname)-8s - %(asctime)s - %(message)s')

# Configuration
DCU_IP = 'dev-si-e2dcu-06.dectris.local'  # Change this to your detector IP
THRESHOLDS = [13000, 30000]  # Two thresholds: 13 keV and 30 keV
OUTPUT_FILE = Path('example_dataset/original_flatfield.tiff')

# Connect and retrieve flatfield
logging.info(f"Connecting to {DCU_IP}...")
c = DEigerClient.DEigerClient(host=DCU_IP)
configure_thresholds(c, THRESHOLDS)

logging.info("Getting flatfield...")
flatfield_response = c.detectorConfig("flatfield")
flatfield_data = flatfield_response["value"]

if flatfield_data is not None and isinstance(flatfield_data, dict):
    if "__darray__" in flatfield_data:
        logging.info("Found darray format flatfield")
        try:
            # Decode the darray format
            flatfield_array = decode_darray(flatfield_data)
            
            # Save to file
            tifffile.imwrite(OUTPUT_FILE, flatfield_array)
            
            logging.info(f"Original flatfield saved to {OUTPUT_FILE}")
            logging.info(f"Final shape: {flatfield_array.shape}")
            logging.info(f"Data type: {flatfield_array.dtype}")
            logging.info(f"Range: {flatfield_array.min():.4f} to {flatfield_array.max():.4f}")
            
        except Exception as e:
            logging.error(f"Error decoding darray: {e}")
    else:
        logging.warning("Flatfield data is not in darray format")
        logging.info(f"Available keys: {list(flatfield_data.keys())}")
else:
    logging.error("No flatfield data available or wrong format")   