import cbor2
from dectris.compression import decompress
import numpy as np
import tifffile
# import datetime
from datetime import datetime
import time 
import argparse
from pathlib import Path
import os
import pprint  # Import pprint for formatting data



def decode_multi_dim_array(tag, column_major):
    dimensions, contents = tag.value
    if isinstance(contents, list):
        array = np.empty((len(contents),), dtype=object)
        array[:] = contents
    elif isinstance(contents, (np.ndarray, np.generic)):
        array = contents
    else:
        raise cbor2.CBORDecodeValueError("expected array or typed array")
    return array.reshape(dimensions, order="F" if column_major else "C")


def decode_typed_array(tag, dtype):
    if not isinstance(tag.value, bytes):
        raise cbor2.CBORDecodeValueError("expected byte string in typed array")
    return np.frombuffer(tag.value, dtype=dtype)


def decode_dectris_compression(tag):
    algorithm, elem_size, encoded = tag.value
    return decompress(encoded, algorithm, elem_size=elem_size)


tag_decoders = {
    40: lambda tag: decode_multi_dim_array(tag, column_major=False),
    64: lambda tag: decode_typed_array(tag, dtype="u1"),
    65: lambda tag: decode_typed_array(tag, dtype=">u2"),
    66: lambda tag: decode_typed_array(tag, dtype=">u4"),
    67: lambda tag: decode_typed_array(tag, dtype=">u8"),
    68: lambda tag: decode_typed_array(tag, dtype="u1"),
    69: lambda tag: decode_typed_array(tag, dtype="<u2"),
    70: lambda tag: decode_typed_array(tag, dtype="<u4"),
    71: lambda tag: decode_typed_array(tag, dtype="<u8"),
    72: lambda tag: decode_typed_array(tag, dtype="i1"),
    73: lambda tag: decode_typed_array(tag, dtype=">i2"),
    74: lambda tag: decode_typed_array(tag, dtype=">i4"),
    75: lambda tag: decode_typed_array(tag, dtype=">i8"),
    77: lambda tag: decode_typed_array(tag, dtype="<i2"),
    78: lambda tag: decode_typed_array(tag, dtype="<i4"),
    79: lambda tag: decode_typed_array(tag, dtype="<i8"),
    80: lambda tag: decode_typed_array(tag, dtype=">f2"),
    81: lambda tag: decode_typed_array(tag, dtype=">f4"),
    82: lambda tag: decode_typed_array(tag, dtype=">f8"),
    83: lambda tag: decode_typed_array(tag, dtype=">f16"),
    84: lambda tag: decode_typed_array(tag, dtype="<f2"),
    85: lambda tag: decode_typed_array(tag, dtype="<f4"),
    86: lambda tag: decode_typed_array(tag, dtype="<f8"),
    87: lambda tag: decode_typed_array(tag, dtype="<f16"),
    1040: lambda tag: decode_multi_dim_array(tag, column_major=True),
    56500: lambda tag: decode_dectris_compression(tag),
}


def tag_hook(decoder, tag):
    tag_decoder = tag_decoders.get(tag.tag)
    return tag_decoder(tag) if tag_decoder else tag


def parseArgs():
    parser = argparse.ArgumentParser()
    parser = argparse.ArgumentParser()
    parser.add_argument("-i","--ip",help="the ip of the detector",default='169.254.254.1', dest='ip')
    parser.add_argument("-d","--direc",help="if you want to save the tiff image, provide the directory",dest='direc', 
                        default='C:/Measurements/temp',type=str)
    parser.add_argument("-l","--live",help="start live viewer", default=False, action="store_true")
    parser.add_argument('-p','--pilatus4',help='if you are using a pilatus4 with 4 thresholds',action="store_true")

    return parser.parse_args()



if __name__ == "__main__":
    args = parseArgs()

    import sys
    import zmq
    import matplotlib.pyplot as plt

    context = zmq.Context()
    endpoint = f"tcp://{args.ip}:31001"
    socket = context.socket(zmq.PULL)


    if args.live:
        plt.ion()
        # set number of images based on threhsolds selected
        if args.pilatus4:
            fig,(ax1,ax2, ax3,ax4) = plt.subplots(2,2,figsize=(20,10))
            ax1.title.set_text ('Threshold 1 image')
            ax2.title.set_text ('Threshold 2 image')
            ax3.title.set_text ('Threshold 3 image')
            ax4.title.set_text ('Threshold 4 image')
            axes = [ax1,ax2,ax3,ax4]
        else:
            fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(12, 7))
            ax1.title.set_text('Threshold 1 image')
            ax2.title.set_text('Threshold 2 image')
            axes = [ax1,ax2]

    with socket.connect(endpoint):
        print(f"PULL {endpoint}")
        while True:
            message = socket.recv()
            message = cbor2.loads(message, tag_hook=tag_hook)
            print(f"========== MESSAGE[{message['type']}] ==========")
            if message['type'] == 'image':
                print(f'processing image {message["image_id"]+1}')
                image_num+=1
                for count, channel in enumerate(channels):
                    data = np.array(message['data'][channel])
                    #data2 = np.array(message['data']['threshold_2'])
                    if args.live:
                        median_counts=np.median(data).astype(np.float32)
                        axes[count].set(title=f'Threshold {thresholds[count]/1e3:.0f} keV: {median_counts:.0f} counts -> {median_counts/count_time/1e6:.3f} Mcps/pixel',
                                        xticks=[], yticks=[])  
                        axim = axes[count].imshow(data, cmap='gray', vmin=median_counts*0.8, vmax=median_counts*1.2)
                        #axim2 = ax2.imshow(data2, cmap='jet', vmin=0, vmax=3)
                        fig.suptitle(f'Image id {message["image_id"]}', fontsize=16)
                    elif args.direc:
                        # tifffile.imwrite(f"{args.direc}/{message['series_unique_id']}_{timestamp}_im{message['image_id']}_{channel}.tif", np.array(message['data'][channel]))
                        filename=f"img_th{count}_{message['image_id']:05d}.tif"
                        tifffile.imwrite(Path(full_path)/filename, np.array(message['data'][channel]))
                if args.live:
                    fig.canvas.flush_events()
            elif message['type'] == 'start':
                #create directory
                working_directory = Path(args.direc)
                acquisition_time = datetime.now().strftime('%Y%m%d_%H%M%S')
                full_path = Path(working_directory, f"{acquisition_time}_Stream") # name of the folder
                print(f"Creating folder {full_path}")
                os.makedirs(full_path)
                
                #save acuisition data
                filename=f"start_data.txt"
                with open(Path(full_path)/filename, 'w') as f:
                    f.write(pprint.pformat(message))

                image_num=0
                count_time=message['count_time']
                thresholds=[message['threshold_energy']['threshold_1'],message['threshold_energy']['threshold_2']]
                print(thresholds)
                start_time=time.time()

                channels = message['channels']
                for val in ['flatfield','pixel_mask']:
                    if val == 'flatfield':
                        for count, channel in enumerate(channels):
                            try:
                                #data1 = np.zeros(np.shape(np.array(message[val]['threshold_1'])),dtype=np.uint32)
                                data = np.zeros(np.shape(np.array(message[val][channel])), dtype=np.uint32)
                                if args.live:
                                    axim = axes[count].imshow(data, cmap='jet', vmin=0, vmax=2)
                                    #axim2 = ax2.imshow(data2, cmap='jet', vmin=0, vmax=2)
                                elif args.direc:
                                    print(count, channel)
                                    # tifffile.imwrite(f"{sys.argv[2]}_{message['series_unique_id']}_{val}_{channel}.tif", np.array(message[val][channel]))
                                    #tifffile.imwrite(f"{sys.argv[2]}_{message['series_unique_id']}_{val}_th2.tif", np.array(message[val]['threshold_2']))

                            except KeyError as e:
                                if e.args[0] in ['flatfield', 'pixel_mask']:
                                    print('there was no pixel mask and/or flatfield, did you enable them with "header_detail" == "all" ?')
                                else:
                                    print('there was no second threshold value to process, did you enable the second threshold in the settings?')
                        if args.live:
                            fig.canvas.flush_events()
            elif message['type'] == 'end':
                elapsed_time=time.time()-start_time
                print(f"{image_num} images in {elapsed_time:.1f} ({int(image_num/elapsed_time):d} Hz) saved in {full_path}")

