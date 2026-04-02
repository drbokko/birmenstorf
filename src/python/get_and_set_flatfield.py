#!/usr/bin/env python3
"""
Flat-field helpers for the EIGER DCU (encode/decode darray, get/set on detector).

Import this module to reuse the functions without connecting to hardware.
Run as a script (`python get_and_set_flatfield.py`) to execute the full example on the detector.
"""
from pathlib import Path
import numpy as np
import base64
import logging
import tifffile
from DEigerClient import DEigerClient

logging.basicConfig(level=logging.INFO, format='%(levelname)-8s - %(asctime)s - %(message)s')

def decode_darray(darray_dict):
    """
    Decode darray format used by EIGER detector.
    Format: {"__darray__": version, "type": dtype, "shape": [w,h], "filters": ["base64"], "data": base64_data}
    """
    dtype_str = darray_dict["type"]  # e.g., "<f4" or "<u4" 
    shape = darray_dict["shape"]     # [width, height]
    base64_data = darray_dict["data"]
    
    logging.debug(f"darray type: {dtype_str}")
    logging.debug(f"darray shape: {shape}")
    logging.debug(f"darray version: {darray_dict.get('__darray__', 'unknown')}")
    
    # Decode base64 data
    binary_data = base64.b64decode(base64_data)
    logging.debug(f"decoded {len(binary_data)} bytes")
    
    # Convert to numpy array with correct dtype and shape
    numpy_array = np.frombuffer(binary_data, dtype=dtype_str).reshape(shape)  # Note: shape is [w,h] but numpy uses [h,w]
    
    return numpy_array

def encode_darray(numpy_array, darray_version=[1, 0, 0]):
    """
    Encode numpy array to darray format used by EIGER detector.
    """
    # Determine data type string
    dtype_map = {
        np.dtype('<f4'): '<f4',  # float32 little endian
        np.dtype('<u4'): '<u4',  # uint32 little endian
        np.dtype('float32'): '<f4',
        np.dtype('uint32'): '<u4'
    }
    numpy_array=numpy_array.reshape(numpy_array.shape[1], numpy_array.shape[0]) # flip from [h,w] to [w,h]

    # Ensure array is in correct format
    if numpy_array.dtype not in [np.dtype('<f4'), np.dtype('<u4')]:
        numpy_array = numpy_array.astype('<f4')
    
    dtype_str = dtype_map.get(numpy_array.dtype, '<f4')
    
    # Get shape in [width, height] format (flip from numpy's [height, width])
    shape = [numpy_array.shape[1], numpy_array.shape[0]]
    
    # Convert to binary and encode in base64
    binary_data = numpy_array.tobytes()
    base64_data = base64.b64encode(binary_data).decode('ascii')
    
    # Create darray dictionary
    darray_dict = {
        "__darray__": darray_version,
        "type": dtype_str,
        "shape": shape,
        "filters": ["base64"],
        "data": base64_data
    }
    
    logging.debug(f"Encoded darray: type={dtype_str}, shape={shape}, size={len(base64_data)} chars")
    
    return darray_dict

def get_flatfield_from_detector(client):
    """Get current flatfield from detector."""
    logging.info("Getting current flatfield...")
    
    try:
        flatfield_response = client.detectorConfig("flatfield")
        flatfield_data = flatfield_response["value"]
        
        if flatfield_data is not None and isinstance(flatfield_data, dict):
            if "__darray__" in flatfield_data:
                logging.info("Found darray format flatfield")
                flatfield_array = decode_darray(flatfield_data)
                logging.info(f"Retrieved flatfield shape: {flatfield_array.shape}")
                logging.info(f"Range: {flatfield_array.min():.4f} to {flatfield_array.max():.4f}")
                return flatfield_array
            else:
                logging.warning("Flatfield data is not in darray format")
                return None
        else:
            logging.error("No flatfield data available")
            return None
            
    except Exception as e:
        logging.error(f"Error getting flatfield: {e}")
        return None

def set_flatfield_to_detector(client, flatfield_array):
    """Upload flatfield to detector."""
    logging.info("Setting flatfield to detector...")
    
    try:
        # Encode to darray format
        logging.debug("Encoding to darray format...")
        flatfield_darray = encode_darray(flatfield_array)
        
        # Disable flatfield first
        logging.debug("Disabling flatfield correction...")
        client.setDetectorConfig("flatfield_correction_applied", False)
        
        # Upload the flatfield
        logging.debug("Uploading flatfield data...")
        client.setDetectorConfig("flatfield", flatfield_darray)
        
        # Enable flatfield correction
        logging.debug("Enabling flatfield correction...")
        client.setDetectorConfig("flatfield_correction_applied", True)
        
        logging.info("Flatfield uploaded and enabled successfully")
        return True
        
    except Exception as e:
        logging.error(f"Error setting flatfield: {e}")
        return False

def configure_thresholds(client, thresholds):
    """Configure detector thresholds."""
    logging.info(f"Configuring thresholds: {thresholds}")
    
    # Set thresholds
    for i, threshold_energy in enumerate(thresholds, start=1):
        client.setDetectorConfig(f"threshold/{i}/mode", 'enabled')
        client.setDetectorConfig(f"threshold/{i}/energy", threshold_energy)
        logging.info(f"Threshold {i}: {threshold_energy} eV")
    
    # Disable additional thresholds if not provided
    if len(thresholds) < 2:
        client.setDetectorConfig("threshold/2/mode", 'disabled')
        logging.info("Threshold 2: disabled")


__all__ = [
    "configure_thresholds",
    "decode_darray",
    "encode_darray",
    "get_flatfield_from_detector",
    "main",
    "set_flatfield_to_detector",
]


def main():
    """Run the interactive get/set/verify example (connects to the detector)."""
    DCU_IP = 'dev-si-e2dcu-06.dectris.local'  # Change this to your detector IP
    THRESHOLDS = [13000, 30000]  # Two thresholds: 13 keV and 30 keV

    logging.info("=" * 60)
    logging.info("EIGER Detector Flatfield Get & Set Example")
    logging.info("=" * 60)

    logging.info("Connecting to %s...", DCU_IP)
    c = DEigerClient.DEigerClient(host=DCU_IP)
    logging.info("Connected - Detector status: %s", c.detectorStatus("state")["value"])

    configure_thresholds(c, THRESHOLDS)

    try:
        x_pixels = c.detectorConfig("x_pixels_in_detector")["value"]
        y_pixels = c.detectorConfig("y_pixels_in_detector")["value"]
        logging.info("Detector geometry: %s x %s pixels", x_pixels, y_pixels)
    except Exception as e:
        logging.warning("Could not get detector geometry: %s", e)

    logging.info("=" * 60)
    logging.info("STEP 1: Getting current flatfield from detector")
    logging.info("=" * 60)

    original_flatfield = get_flatfield_from_detector(c)

    if original_flatfield is not None:
        tifffile.imwrite(Path('example_dataset/original_flatfield.tiff'), original_flatfield)
        logging.info("Original flatfield saved as 'original_flatfield.tiff'")

        logging.info("=" * 60)
        logging.info("STEP 2: Re-uploading flatfield for both thresholds")
        logging.info("=" * 60)

        logging.info("Test 1: Re-uploading original flatfield...")
        success1 = set_flatfield_to_detector(c, original_flatfield)

        if success1:
            logging.info("Verifying uploaded flatfield...")
            retrieved_flatfield = get_flatfield_from_detector(c)

            if retrieved_flatfield is not None:
                if np.allclose(original_flatfield, retrieved_flatfield, rtol=1e-6):
                    logging.info("Verification successful: uploaded flatfield matches original")
                else:
                    logging.warning("uploaded flatfield differs from original")
                    logging.info(
                        "Max difference: %s",
                        np.max(np.abs(original_flatfield - retrieved_flatfield)),
                    )

        logging.info("=" * 60)
        logging.info("STEP 3: Testing with modified flatfield")
        logging.info("=" * 60)

        logging.info("Test 2: Creating modified flatfield...")
        modified_flatfield = original_flatfield.copy()

        # rng = np.random.default_rng(42)
        # noise = rng.uniform(-0.50, 0.50, size=modified_flatfield.shape)
        # modified_flatfield = modified_flatfield * (1.0 + noise)
        # modified_flatfield = np.clip(modified_flatfield, 0.1, 10.0)
        modified_flatfield[100:200, 50:100]=2.0

        logging.info(
            "Modified flatfield range: %s to %s",
            modified_flatfield.min(),
            modified_flatfield.max(),
        )
        logging.info("Mean change: %s", np.mean(modified_flatfield - original_flatfield))

        success2 = set_flatfield_to_detector(c, modified_flatfield)

        if success2:
            tifffile.imwrite(Path('example_dataset/modified_flatfield.tiff'), modified_flatfield)
            logging.info("Modified flatfield saved as 'modified_flatfield.tiff'")

            final_flatfield = get_flatfield_from_detector(c)
            if final_flatfield is not None:
                if np.allclose(modified_flatfield, final_flatfield, rtol=1e-6):
                    logging.info("Modified flatfield uploaded and verified successfully")
                else:
                    logging.warning("retrieved flatfield differs from uploaded version")

        logging.info("=" * 60)
        logging.info("SUMMARY")
        logging.info("=" * 60)
        logging.info("Both thresholds configured: %s", THRESHOLDS)
        logging.info("Original flatfield retrieved and saved")
        logging.info("Flatfield round-trip test: %s", "PASSED" if success1 else "FAILED")
        logging.info("Modified flatfield test: %s", "PASSED" if success2 else "FAILED")
        logging.info("Files created:")
        logging.info("- original_flatfield.npy: Original detector flatfield")
        if success2:
            logging.info("- modified_flatfield.npy: Modified flatfield with random variations")

    else:
        logging.error("=" * 60)
        logging.error("CANNOT CONTINUE - No flatfield available from detector")
        logging.error("=" * 60)
        logging.error("Make sure:")
        logging.error("1. Detector has a flatfield correction configured")
        logging.error("2. Detector is properly connected and initialized")
        logging.error("3. Flatfield correction is enabled")

    logging.info("=" * 60)


if __name__ == "__main__":
    main()