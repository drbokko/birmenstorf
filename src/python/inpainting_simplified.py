"""
NOT TESTED
Simple script to inpaint bad pixels and gaps in a TIFF stack
a custom mask is used to identify the bad pixels and gaps (horizontal, vertical, intersection)
"""
import os
import time
import numpy as np
import multiprocessing
from scipy.ndimage import uniform_filter1d
from scipy.interpolate import interp1d
from pathlib import Path
import tifffile as tiff
from silx.math.medianfilter import medfilt2d


# ---------------- Frame processing ----------------
def process_frame(frame_file, frame_index, input_folder, output_folder, horizontal_gap_mask, vertical_gap_mask, intersection_mask, sparse_mask, clustered_mask, debug_mode=False):
        
    #load frame
    if debug_mode: print(f"Processing {frame_file} ...")
    frame = tiff.imread(os.path.join(input_folder, frame_file))
    bit_frame = frame.dtype.itemsize * 8
    if bit_frame ==16:
        frame = frame.astype(np.float32) # Convert to float for processing, will convert back to uint16 at the end
        if debug_mode: print(f"Loaded 16-bit frame, converted to float for processing")
    detector_height, detector_width = frame.shape

    start_time_total = time.time()

    def check_invalid(f, excluded_mask=None, included_mask=None):
        invalid_pixels = (np.isnan(f) | np.isinf(f) | (f>65530) ) & (~excluded_mask if excluded_mask is not None else True)
        if included_mask is not None:
            invalid_pixels = np.logical_or(invalid_pixels, included_mask)
        num_invalid_pixels = np.count_nonzero(invalid_pixels)
        return num_invalid_pixels, invalid_pixels

    # remove NaN/Inf/outliers/sparse-pixels/clusters using median filter
    radius_outlier = 15
    threshold_outlier = 100

    frame_median=medfilt2d(frame, kernel_size=radius_outlier)
    outliers = np.abs(frame - frame_median) > threshold_outlier
    num_invalid_remaining, invalid = check_invalid(frame, excluded_mask= horizontal_gap_mask | vertical_gap_mask | intersection_mask,
                                                   included_mask=outliers | sparse_mask | clustered_mask)

    if debug_mode: print(f"Frame {frame_index+1} - Clean  bad-pixels starting:  \t{num_invalid_remaining} pixels remaining")
    iterations = 0
    while num_invalid_remaining and iterations < 5:
        step_start = time.time()
        iterations += 1
        frame[invalid] = frame_median[invalid]

        frame_median=medfilt2d(frame, kernel_size=radius_outlier)
        outliers = np.abs(frame - frame_median) > threshold_outlier
        num_invalid_remaining, invalid = check_invalid(frame, excluded_mask= horizontal_gap_mask | vertical_gap_mask | intersection_mask,
                                                       included_mask=outliers)

        if debug_mode: print(f"Frame {frame_index+1} - \t\t (iter# {iterations}): \t{time.time()-step_start:.3f}s \t{num_invalid_remaining} pixels remaining")
    if iterations == 5 and num_invalid_remaining:
        print(f"Warning: Frame {frame_index+1} - Maximum iterations reached with {num_invalid_remaining} pixels still invalid! adjust the threshold_outlier or radius_outlier parameters")

    # Vertical gap correction - vectorized approach
    step_start = time.time()
    num_vertical = np.count_nonzero(vertical_gap_mask)
    for y in range(detector_height):
        row = frame[y, :]
        row_mask = vertical_gap_mask[y, :]
        
        if not np.any(row_mask):
            continue
        
        # Find valid (non-gap) positions
        valid_positions = np.where(~row_mask)[0]
        gap_positions = np.where(row_mask)[0]
        
        if len(valid_positions) < 2:
            continue
        
        # Interpolate gap values from valid neighbors
        interp_func = interp1d(valid_positions, row[valid_positions], 
                              kind='linear', fill_value='extrapolate')
        frame[y, gap_positions] = interp_func(gap_positions)
    if debug_mode: print(f"Frame {frame_index+1} - Vertical gap correction: \t{time.time()-step_start:.3f}s \t{num_vertical} pixels corrected")

    # Horizontal gap correction - vectorized approach
    step_start = time.time()
    num_horizontal = np.count_nonzero(horizontal_gap_mask)
    for x in range(detector_width):
        col = frame[:, x]
        col_mask = horizontal_gap_mask[:, x]
        
        if not np.any(col_mask):
            continue
        
        # Find valid (non-gap) positions
        valid_positions = np.where(~col_mask)[0]
        gap_positions = np.where(col_mask)[0]
        
        if len(valid_positions) < 2:
            continue
        
        # Interpolate gap values from valid neighbors
        interp_func = interp1d(valid_positions, col[valid_positions], 
                              kind='linear', fill_value='extrapolate')
        frame[gap_positions, x] = interp_func(gap_positions)
    if debug_mode: print(f"Frame {frame_index+1} - Horizontal gap correction: \t{time.time()-step_start:.3f}s \t{num_horizontal} pixels corrected")

    # Intersection correction
    step_start = time.time()
    num_intersection = np.count_nonzero(intersection_mask)
    frame[intersection_mask] = medfilt2d(frame, kernel_size=10)[intersection_mask]
    if debug_mode: print(f"Frame {frame_index+1} - Intersection correction: \t{time.time()-step_start:.3f}s \t{num_intersection} pixels corrected")

    # Vertical smoothing - using gap information
    step_start = time.time()
    smoothed_vertically = uniform_filter1d(frame, size=5, axis=0, mode="nearest")
    frame[vertical_gap_mask] = smoothed_vertically[vertical_gap_mask]
    if debug_mode: print(f"Frame {frame_index+1} - Vertical smoothing: \t\t{time.time()-step_start:.3f}s")

    # Horizontal smoothing - using gap information
    step_start = time.time()
    smoothed_horizontally = uniform_filter1d(frame, size=5, axis=1, mode="nearest")
    frame[horizontal_gap_mask] = smoothed_horizontally[horizontal_gap_mask] 
    if debug_mode: print(f"Frame {frame_index+1} - Horizontal smoothing: \t{time.time()-step_start:.3f}s")


    num_invalid_remaining, invalid = check_invalid(frame)
    if debug_mode: 
        print(f"Frame {frame_index+1} - TOTAL: \t\t\t{time.time()-start_time_total:.3f}s\t{num_invalid_remaining}  Remaining invalid pixels")
    else:
        print(f"Frame {frame_file} - TOTAL: \t\t\t{time.time()-start_time_total:.3f}s\t{num_invalid_remaining}  Remaining invalid pixels")


    # Save frame as raw file
    if debug_mode:
        raw_output_path = os.path.join(output_folder, f"{frame_file}_{detector_width}x{detector_height}x1.raw")
        frame.astype(np.float32).tofile(raw_output_path)
        print(f"Saved raw file: {raw_output_path}")
    if bit_frame ==16:
        frame = frame.astype(np.uint16)
    tiff.imwrite(os.path.join(output_folder, frame_file), frame)
    
    return 



# ---------------- Main ----------------
if __name__ == "__main__":
    # === User parameters ===
    debug_mode = False

    input_folder = "C:\\Measurements\\SANTIS_ME_DP-P086-10\\20260204_162749_Dy" 
    mask_folder = "C:\\Measurements\\SANTIS_ME_DP-P086-10\\"
    mask_name=f"bad_pixel_mask_SANTIS_ME_DP-P086-10"

    # Check input stack
    print(f"Processing frames from {input_folder}..")
    tif_files = sorted([f for f in os.listdir(input_folder) if f.endswith('.tif') or f.endswith('.tiff')])
    if len(tif_files) == 0:
        raise ValueError(f"No TIF files found in {input_folder}")
    num_frames = len(tif_files)
    if debug_mode:
        num_frames=1       
    print(f"Found {len(tif_files)} TIF files, loading {num_frames} frames")


    # Load masks
    mask_filename  = mask_folder + f"{mask_name}.tiff"
    print(f"Loading mask from {mask_filename} ")
    mask = tiff.imread(mask_filename)
    horizontal_gap_mask= np.logical_or(mask == 1, mask == 6)
    vertical_gap_mask= np.logical_or(mask == 2, mask == 6)
    intersection_mask= (mask == 3)
    sparse_mask= (mask == 4)
    clustered_mask= (mask == 5)

    # Create output folder
    output_folder = input_folder + "_BP"
    os.makedirs(output_folder, exist_ok=True)   

    # Parallel processing: Prepare args per frame
    args = [
        (tif_files[i], i, input_folder, output_folder, horizontal_gap_mask, vertical_gap_mask, intersection_mask, sparse_mask, clustered_mask, debug_mode)
          for i in range(num_frames)]
    num_cores = 1 if debug_mode else multiprocessing.cpu_count()
    print(f"Using {num_cores} cores")
    start_processing_time = time.time()
    with multiprocessing.Pool(processes=num_cores) as pool:
        pool.starmap(process_frame, args)
    
    print(f"Total processing time: {(time.time() - start_processing_time):.1f}s for {num_frames} frames")
    print(f"Processing time: {(time.time() - start_processing_time)/num_frames*1000:.1f} ms/frame")


