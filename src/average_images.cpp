/*
 * average_images — mean of all single-channel TIFFs in a directory (non-recursive).
 *
 * Usage:
 *   average_images <input_folder> <output.tif>
 *
 * Reads every *.tif / *.tiff in the folder (same size and type family as first image).
 * Writes one float32 grayscale TIFF (pixel-wise arithmetic mean).
 *
 * Requires LibTIFF (e.g. libtiff-dev on Debian/Ubuntu).
 */

#include <tiffio.h>

#include "tiff_warning_filter.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static bool ends_with_tiff(const fs::path& p) {
    std::string e = p.extension().string();
    std::transform(e.begin(), e.end(), e.begin(), ::tolower);
    return e == ".tif" || e == ".tiff";
}

static bool load_tiff_grayscale_float(const char* path,
                                    std::vector<float>& out,
                                    uint32_t& w,
                                    uint32_t& h) {
    TIFF* tif = TIFFOpen(path, "r");
    if (!tif)
        return false;

    uint32_t imgw = 0, imgh = 0;
    TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &imgw);
    TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &imgh);
    if (imgw == 0 || imgh == 0) {
        TIFFClose(tif);
        return false;
    }

    uint16_t spp = 1;
    TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLESPERPIXEL, &spp);
    if (spp != 1) {
        std::fprintf(stderr, "%s: expected 1 sample per pixel, got %u\n", path, (unsigned)spp);
        TIFFClose(tif);
        return false;
    }

    uint16_t bps = 8;
    TIFFGetFieldDefaulted(tif, TIFFTAG_BITSPERSAMPLE, &bps);

    uint16_t sf = SAMPLEFORMAT_UINT;
    TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLEFORMAT, &sf);

    w = imgw;
    h = imgh;
    out.resize((size_t)w * h);

    if (bps == 32 && sf == SAMPLEFORMAT_IEEEFP) {
        for (uint32_t row = 0; row < h; ++row) {
            if (TIFFReadScanline(tif, out.data() + (size_t)row * w, row, 0) < 0) {
                TIFFClose(tif);
                return false;
            }
        }
    } else if (bps == 16) {
        std::vector<uint16_t> line(w);
        for (uint32_t row = 0; row < h; ++row) {
            if (TIFFReadScanline(tif, line.data(), row, 0) < 0) {
                TIFFClose(tif);
                return false;
            }
            for (uint32_t x = 0; x < w; ++x)
                out[(size_t)row * w + x] = static_cast<float>(line[x]);
        }
    } else if (bps == 8) {
        std::vector<uint8_t> line(w);
        for (uint32_t row = 0; row < h; ++row) {
            if (TIFFReadScanline(tif, line.data(), row, 0) < 0) {
                TIFFClose(tif);
                return false;
            }
            for (uint32_t x = 0; x < w; ++x)
                out[(size_t)row * w + x] = static_cast<float>(line[x]);
        }
    } else {
        std::fprintf(stderr, "%s: unsupported bps=%u sampleformat=%u\n", path, (unsigned)bps,
                     (unsigned)sf);
        TIFFClose(tif);
        return false;
    }

    TIFFClose(tif);
    return true;
}

static bool save_tiff_float32(const char* path, const float* data, uint32_t w, uint32_t h) {
    TIFF* tif = TIFFOpen(path, "w");
    if (!tif)
        return false;

    TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, w);
    TIFFSetField(tif, TIFFTAG_IMAGELENGTH, h);
    TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 1);
    TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 32);
    TIFFSetField(tif, TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_IEEEFP);
    TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK);
    TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
    TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, h);

    for (uint32_t row = 0; row < h; ++row) {
        if (TIFFWriteScanline(tif, (void*)(data + (size_t)row * w), row, 0) < 0) {
            TIFFClose(tif);
            return false;
        }
    }
    TIFFClose(tif);
    return true;
}

static void print_usage(const char* prog) {
    std::fprintf(stderr,
                 "usage: %s <input_folder> <output.tif>\n"
                 "  Averages all .tif/.tiff in the folder (not subfolders).\n",
                 prog);
}

int main(int argc, char** argv) {
    dectris::install_libtiff_warning_filter();
    if (argc != 3) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    const fs::path input_dir = fs::absolute(argv[1]);
    const fs::path out_path = fs::absolute(argv[2]);

    if (!fs::is_directory(input_dir)) {
        std::fprintf(stderr, "error: not a directory: %s\n", input_dir.string().c_str());
        return EXIT_FAILURE;
    }

    std::vector<fs::path> files;
    for (const fs::directory_entry& entry : fs::directory_iterator(input_dir)) {
        if (!entry.is_regular_file())
            continue;
        const fs::path p = entry.path();
        if (!ends_with_tiff(p))
            continue;
        files.push_back(p);
    }
    std::sort(files.begin(), files.end());

    if (files.empty()) {
        std::fprintf(stderr, "error: no TIFF files in %s\n", input_dir.string().c_str());
        return EXIT_FAILURE;
    }

    std::vector<float> img;
    uint32_t w0 = 0, h0 = 0;
    if (!load_tiff_grayscale_float(files[0].string().c_str(), img, w0, h0)) {
        std::fprintf(stderr, "error: failed to read %s\n", files[0].string().c_str());
        return EXIT_FAILURE;
    }

    const size_t npx = (size_t)w0 * h0;
    std::vector<double> sum(npx);
    for (size_t i = 0; i < npx; ++i)
        sum[i] = static_cast<double>(img[i]);

    int used = 1;
    for (size_t fi = 1; fi < files.size(); ++fi) {
        uint32_t w = 0, h = 0;
        if (!load_tiff_grayscale_float(files[fi].string().c_str(), img, w, h)) {
            std::fprintf(stderr, "warn: skip unreadable %s\n", files[fi].string().c_str());
            continue;
        }
        if (w != w0 || h != h0) {
            std::fprintf(stderr, "warn: skip %s (size %ux%u != %ux%u)\n",
                         files[fi].filename().string().c_str(), w, h, w0, h0);
            continue;
        }
        for (size_t i = 0; i < npx; ++i)
            sum[i] += static_cast<double>(img[i]);
        used++;
    }

    std::vector<float> avg(npx);
    const double inv = 1.0 / static_cast<double>(used);
    for (size_t i = 0; i < npx; ++i)
        avg[i] = static_cast<float>(sum[i] * inv);

    {
        std::error_code ec;
        fs::create_directories(out_path.parent_path(), ec);
    }

    if (!save_tiff_float32(out_path.string().c_str(), avg.data(), w0, h0)) {
        std::fprintf(stderr, "error: failed to write %s\n", out_path.string().c_str());
        return EXIT_FAILURE;
    }

    std::printf("wrote %s (mean of %d images, %zu skipped)\n", out_path.string().c_str(), used,
                files.size() - (size_t)used);
    return EXIT_SUCCESS;
}
