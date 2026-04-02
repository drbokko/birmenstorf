#!/usr/bin/env python3
"""
Load a processed flat field from TIFF or .npy (e.g. from process_flatfield.py),
upload it to the EIGER DCU, then read it back and verify it matches.

Uses set_flatfield_to_detector / get_flatfield_from_detector from get_and_set_flatfield.py.
"""

import argparse
import logging
import sys
from pathlib import Path

import numpy as np
from DEigerClient import DEigerClient
from get_and_set_flatfield import get_flatfield_from_detector, set_flatfield_to_detector, configure_thresholds

import tifffile

logging.basicConfig(level=logging.DEBUG, format="%(levelname)-8s - %(asctime)s - %(message)s")



def main() -> int:
    THRESHOLDS = [13000, 30000]  # Two thresholds: 13 keV and 30 keV

    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument(
        "flatfield_file",
        type=Path,
        help="processed flat field to upload (.npy, .tif, .tiff)",
    )
    p.add_argument("--host", default="dev-si-e2dcu-06.dectris.local", help="DCU hostname or IP")
    p.add_argument(
        "--no-verify",
        action="store_true",
        help="upload only, skip read-back verification",
    )
    args = p.parse_args()

    # load the custom flatfield from file
    logging.info("Loading custom flatfield from %s", args.flatfield_file)
    try:
        flat = tifffile.imread(args.flatfield_file)
    except Exception as e:
        logging.error("Failed to load file: %s", e)
        return 2

    logging.info("Loaded shape=%s dtype=%s range=[%s, %s]",
                 flat.shape, flat.dtype, np.min(flat), np.max(flat))

    # connect to the DCU
    logging.info("Connecting to DCU %s...", args.host)
    client = DEigerClient.DEigerClient(host=args.host)
    try:
        state = client.detectorStatus("state")["value"]
        logging.info("Detector state: %s", state)
        if state != 'idle':
            logging.info("Initializing detector...")
            client.sendDetectorCommand("initialize")
    except Exception as e:
        logging.warning("Could not read detector state: %s", e)
    configure_thresholds(client, THRESHOLDS)

    # Get current flatfield from the DCU
    original_flatfield = get_flatfield_from_detector(client)    

    #check if the original flatfield has the same shape as the new flatfield
    if original_flatfield.shape != flat.shape:
        logging.error("Original flatfield shape %s does not match new flatfield shape %s", original_flatfield.shape, flat.shape)
        flat=flat[1:-1, 1:-1]
        logging.info("Cropped flatfield shape to %s", flat.shape)

    if not set_flatfield_to_detector(client, flat):
        logging.error("Upload to DCU failed")
        return 1

    if args.no_verify:
        logging.info("Skipping verification (--no-verify)")
        return 0


if __name__ == "__main__":
    sys.exit(main())
