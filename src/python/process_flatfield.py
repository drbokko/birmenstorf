#!/usr/bin/env python3
"""
Load a flat-field TIFF, convert to float32, divide by its median,
take the reciprocal, and save the result as TIFF.
Requires: pip install tifffile numpy
"""

import argparse
import logging
from pathlib import Path

import numpy as np
import tifffile

logging.basicConfig(level=logging.INFO, format="%(levelname)-8s - %(asctime)s - %(message)s")


def process_flatfield(path_in: Path, path_out: Path) -> None:
    flat = tifffile.imread(path_in)
    f32 = np.asarray(flat, dtype=np.float32)


    med = float(np.median(f32))
    if med == 0.0 or not np.isfinite(med):
        raise ValueError(f"cannot normalize: median is {med!r}")

    normalized = f32 / med
    reciprocal = (1.0 / normalized).astype(np.float32)
    reciprocal[np.isinf(reciprocal)] = 0.0
    reciprocal[np.isnan(reciprocal)] = 0.0

    path_out.parent.mkdir(parents=True, exist_ok=True)
    tifffile.imwrite(path_out, reciprocal)

    logging.info("loaded %s shape=%s dtype=%s", path_in, flat.shape, flat.dtype)
    logging.info("median(before norm)=%s", med)
    logging.info("saved %s shape=%s dtype=float32 range=[%s, %s]",
                 path_out, reciprocal.shape, reciprocal.min(), reciprocal.max())


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("input", type=Path, help="input flat field (.tif / .tiff)")
    p.add_argument(
        "-o", "--output",
        type=Path,
        default=None,
        help="output TIFF (default: <input_stem>_reciprocal.tif next to input)",
    )
    args = p.parse_args()

    path_in = args.input
    if not path_in.is_file():
        raise SystemExit(f"input not found: {path_in}")

    path_out = args.output
    if path_out is None:
        path_out = path_in.with_name(f"{path_in.stem}_reciprocal.tif")

    process_flatfield(path_in, path_out)


if __name__ == "__main__":
    main()
