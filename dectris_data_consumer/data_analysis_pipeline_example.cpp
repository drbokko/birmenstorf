/*
 * data_analysis_pipeline_example.cpp — sample offline pipeline on a folder of TIFFs.
 *
 * Usage:
 *   data_analysis_pipeline_example <input_folder> <flatfield.tif>
 *
 * Loads every *.tif / *.tiff in the input folder (non-recursive), divides each image
 * by the flatfield (pixel-wise). Where the flatfield is zero, the output pixel is zero.
 * Two dummy processing stages run after flatfield correction. Results are written as
 * float32 TIFFs under <input_folder>/processed/.
 *
 * Requires LibTIFF (e.g. libtiff-dev on Debian/Ubuntu).
 */

#include <tiffio.h>

#include <algorithm>
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

/* Load single-channel image as float32 (uint8/uint16 scaled to float, or float32). */
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

/* --- Dummy pipeline stages (replace with real algorithms) --- */

class FlatfieldCorrector {
public:
    explicit FlatfieldCorrector(std::vector<float> flatfield)
        : flatfield_(std::move(flatfield)) {}

    void apply(std::vector<float>& image) const {
        if (image.size() != flatfield_.size())
            return;
        for (size_t i = 0; i < image.size(); ++i) {
            const float d = flatfield_[i];
            image[i] = (d != 0.0f) ? (image[i] / d) : 0.0f;
        }
    }

private:
    std::vector<float> flatfield_;
};

class DummyOffsetAndScale {
public:
    void apply(std::vector<float>& image) const {
        for (float& v : image)
            v = v * 1.0001f + 1e-6f; /* mock linear stage */
    }
};

class DummyClipStage {
public:
    void apply(std::vector<float>& image) const {
        for (float& v : image) {
            if (v < 0.0f)
                v = 0.0f;
            if (v > 1.0e9f)
                v = 1.0e9f; /* mock dynamic range limit */
        }
    }
};

static void print_usage(const char* prog) {
    std::fprintf(stderr,
                 "usage: %s <input_folder> <flatfield.tif>\n"
                 "  Reads all .tif/.tiff in input_folder (not in subfolders).\n"
                 "  Divides each image by flatfield (0 in flatfield -> 0 out).\n"
                 "  Writes float32 TIFFs to <input_folder>/processed/\n",
                 prog);
}

int main(int argc, char** argv) {
    if (argc != 3) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    const fs::path input_dir = fs::absolute(argv[1]);
    const fs::path flat_path = fs::absolute(argv[2]);

    if (!fs::is_directory(input_dir)) {
        std::fprintf(stderr, "error: not a directory: %s\n", input_dir.string().c_str());
        return EXIT_FAILURE;
    }
    if (!fs::is_regular_file(flat_path)) {
        std::fprintf(stderr, "error: flatfield not a file: %s\n", flat_path.string().c_str());
        return EXIT_FAILURE;
    }

    std::vector<float> flat_data;
    uint32_t fw = 0, fh = 0;
    if (!load_tiff_grayscale_float(flat_path.string().c_str(), flat_data, fw, fh)) {
        std::fprintf(stderr, "error: failed to load flatfield %s\n", flat_path.string().c_str());
        return EXIT_FAILURE;
    }

    const fs::path out_dir = input_dir / "processed";
    std::error_code ec;
    fs::create_directories(out_dir, ec);
    if (ec) {
        std::fprintf(stderr, "error: cannot create %s: %s\n", out_dir.string().c_str(),
                     ec.message().c_str());
        return EXIT_FAILURE;
    }

    FlatfieldCorrector corrector(std::move(flat_data));
    DummyOffsetAndScale stage_a;
    DummyClipStage stage_b;

    int processed = 0;
    int skipped = 0;

    for (const fs::directory_entry& entry : fs::directory_iterator(input_dir)) {
        if (!entry.is_regular_file())
            continue;
        const fs::path p = entry.path();
        if (!ends_with_tiff(p))
            continue;
        {
            std::error_code ec_same;
            if (fs::equivalent(p, flat_path, ec_same)) {
                skipped++;
                continue;
            }
        }
        /* Skip anything already under processed/ if user symlinked oddly */
        if (p.parent_path().filename() == "processed")
            continue;

        std::vector<float> img;
        uint32_t w = 0, h = 0;
        if (!load_tiff_grayscale_float(p.string().c_str(), img, w, h)) {
            std::fprintf(stderr, "warn: skip unreadable %s\n", p.string().c_str());
            skipped++;
            continue;
        }
        if (w != fw || h != fh) {
            std::fprintf(stderr, "warn: skip %s (size %ux%u != flatfield %ux%u)\n",
                         p.filename().string().c_str(), w, h, fw, fh);
            skipped++;
            continue;
        }

        corrector.apply(img);
        stage_a.apply(img);
        stage_b.apply(img);

        const fs::path dest = out_dir / p.filename();
        if (!save_tiff_float32(dest.string().c_str(), img.data(), w, h)) {
            std::fprintf(stderr, "error: failed to write %s\n", dest.string().c_str());
            return EXIT_FAILURE;
        }
        processed++;
        std::printf("wrote %s\n", dest.string().c_str());
    }

    std::printf("done: %d images written to %s/ (%d skipped)\n", processed,
                 out_dir.string().c_str(), skipped);
    return EXIT_SUCCESS;
}
