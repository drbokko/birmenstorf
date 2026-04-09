import hdf5plugin
import h5py
import requests
import math
import numpy as np
from pathlib import Path
import tifffile


def download_file(url, filename):
    print(f"downloading {url}")
    with requests.get(url, stream=True) as r:
        r.raise_for_status()
        with open(filename, "wb") as f:
            for chunk in r.iter_content(chunk_size=8192):
                if chunk:    # filter out keep-alive new chunks
                    f.write(chunk)
                    

def download_hdf5_save_images(
    dcu_host,
    hdf5_file_root_name,
    nimages_per_file,
    n_images,
    save_path,
    save_raw
):
    #for filewriter2
    masterfile_name = f"{hdf5_file_root_name}_master.h5"
    masterfile_full_url = f"http://{dcu_host}/data/{masterfile_name}"
    download_file(masterfile_full_url, Path(save_path, masterfile_name))
        
    if nimages_per_file > 0:
        datafile_names = [f"{hdf5_file_root_name}_data_{datafile_counter:06}.h5"
                      for datafile_counter in range(1, math.ceil(n_images / nimages_per_file) + 1, 1)]
        for datafile_name in datafile_names:
            datafile_full_url = f"http://{dcu_host}/data/{datafile_name}"
            download_file(datafile_full_url, Path(save_path, datafile_name))
        all_images = h5py.File(Path(save_path, masterfile_name), 'r')["entry"]["data"]['data']
        for th in range(all_images.shape[1]):
            images=all_images[:,th,:,:]
            if save_raw:
                 print('Saving raw')
                 temp=np.array(images, dtype=np.uint16)
                 temp.tofile(Path(save_path, f"img_th{th+1:1d}_{np.shape(images)[2]}x{np.shape(images)[1]}x{np.shape(images)[0]}.raw"))
            else:
                print('Saving tif')
                image_num = 0
                for image in images:
                    tifffile.imwrite(Path(save_path, f"img_th{th+1:1d}_{image_num:05d}.tif"), image)
                    image_num = image_num + 1
    else:
        test_images = h5py.File(masterfile_name, 'r')["entry"]["data"]['data']
        for image in test_images:
            print(np.sum(image))
