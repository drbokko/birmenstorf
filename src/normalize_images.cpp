// =============================================================================
// normalize_tiff — EXAMPLE / DEMONSTRATION CODE ONLY
//
// Not production software; not supported. Replace with validated tools for your
// site if required.
//
// What it does (plain English):
//   - Loads ONE flat-field image (usually 32-bit float TIFF).
//   - Loads MANY object/projection images (usually 16-bit unsigned TIFF).
//   - For each object image, computes:  transmission = object / (flat + epsilon)
//   - Saves results as 32-bit float TIFF files.
//
// Next step in the full pipeline (optional): inpaint_tiff on these outputs.
//
// Why "epsilon":
//   - Protects against division by zero if a flat-field pixel is exactly zero.
//
// Build (with this repository's CMake):
//   cmake -S dectris_data_consumer -B build
//   cmake --build build --target normalize_images
//   (needs OpenCV: libopencv-dev on Debian/Ubuntu, or -DOpenCV_DIR=... on Windows)
//
// This file is intentionally verbose so non-programmers can follow it.
// =============================================================================

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// -----------------------------------------------------------------------------
// Small helper: print how to use this program if the user passes wrong arguments.
// -----------------------------------------------------------------------------
static void print_usage(const char *exe_name) {
    std::cerr
        << "Usage:\n"
        << "  " << exe_name
        << " --flat <flat.tif> --in <folder_with_object_tiffs> --out <output_folder>\n"
        << "Optional:\n"
        << "  --epsilon <number>   (default: 1e-6) added to flat before divide\n";
}

// -----------------------------------------------------------------------------
// Returns true if the file extension looks like a TIFF we want to read.
// -----------------------------------------------------------------------------
static bool is_tiff_extension(const fs::path &p) {
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext == ".tif" || ext == ".tiff";
}

// -----------------------------------------------------------------------------
// Read every TIFF path in a folder, sort by name, return full paths.
// We sort so frame order is stable (img_00001 before img_00002, etc.).
// -----------------------------------------------------------------------------
static std::vector<fs::path> list_tiffs_sorted(const fs::path &folder) {
    std::vector<fs::path> out;
    if (!fs::exists(folder) || !fs::is_directory(folder)) {
        return out;
    }
    for (const auto &entry : fs::directory_iterator(folder)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        if (is_tiff_extension(entry.path())) {
            out.push_back(entry.path());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

// -----------------------------------------------------------------------------
// Load the flat-field image as 32-bit float (CV_32F).
// If the file is 16-bit, we convert to float. If it is already float, we keep it.
// -----------------------------------------------------------------------------
static bool load_flat_as_float32(const fs::path &path, cv::Mat *out_flat32) {
    cv::Mat img = cv::imread(path.string(), cv::IMREAD_UNCHANGED);
    if (img.empty()) {
        std::cerr << "Error: could not read flat field file: " << path << "\n";
        return false;
    }
    if (img.channels() != 1) {
        std::cerr << "Error: expected single-channel TIFF for flat field.\n";
        return false;
    }
    if (img.type() == CV_32FC1) {
        *out_flat32 = img;
        return true;
    }
    if (img.type() == CV_16UC1) {
        img.convertTo(*out_flat32, CV_32F);
        return true;
    }
    std::cerr << "Error: flat field type not supported (use 16U or 32F).\n";
    return false;
}

// -----------------------------------------------------------------------------
// Load an object image as 32-bit float (for division).
// -----------------------------------------------------------------------------
static bool load_object_as_float32(const fs::path &path, cv::Mat *out_obj32) {
    cv::Mat img = cv::imread(path.string(), cv::IMREAD_UNCHANGED);
    if (img.empty()) {
        std::cerr << "Error: could not read object image: " << path << "\n";
        return false;
    }
    if (img.channels() != 1) {
        std::cerr << "Error: expected single-channel TIFF for object image.\n";
        return false;
    }
    img.convertTo(*out_obj32, CV_32F);
    return true;
}

// -----------------------------------------------------------------------------
// Check that two images have the same width and height.
// -----------------------------------------------------------------------------
static bool same_size(const cv::Mat &a, const cv::Mat &b) {
    return a.rows == b.rows && a.cols == b.cols;
}

// Output stem: put "_norm" before a trailing digit run (e.g. img_00001 -> img_norm_00001).
// If the stem has no trailing digits, append "_norm" as before (e.g. frame -> frame_norm).
static std::string normalized_output_stem(const fs::path &obj_path) {
    std::string stem = obj_path.stem().string();
    std::size_t j = stem.size();
    while (j > 0 && std::isdigit(static_cast<unsigned char>(stem[j - 1]))) {
        --j;
    }
    if (j == stem.size()) {
        return stem + "_norm";
    }
    std::string number = stem.substr(j);
    std::string prefix = stem.substr(0, j);
    while (!prefix.empty() && prefix.back() == '_') {
        prefix.pop_back();
    }
    if (prefix.empty()) {
        return std::string("_norm_") + number;
    }
    return prefix + "_norm_" + number;
}

int main(int argc, char **argv) {
    fs::path flat_path;
    fs::path in_folder;
    fs::path out_folder;
    double epsilon = 1e-6;

    // -------------------------------------------------------------------------
    // Parse command line (very simple manual parsing — easy to read).
    // Expected pairs: --flat path --in path --out path [--epsilon number]
    // -------------------------------------------------------------------------
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--flat" && i + 1 < argc) {
            flat_path = argv[++i];
        } else if (arg == "--in" && i + 1 < argc) {
            in_folder = argv[++i];
        } else if (arg == "--out" && i + 1 < argc) {
            out_folder = argv[++i];
        } else if (arg == "--epsilon" && i + 1 < argc) {
            epsilon = std::strtod(argv[++i], nullptr);
        } else if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else {
            std::cerr << "Unknown or incomplete argument: " << arg << "\n";
            print_usage(argv[0]);
            return 1;
        }
    }

    if (flat_path.empty() || in_folder.empty() || out_folder.empty()) {
        print_usage(argv[0]);
        return 1;
    }

    // Create output folder if it does not exist yet.
    std::error_code ec;
    fs::create_directories(out_folder, ec);
    if (ec) {
        std::cerr << "Error: could not create output folder: " << out_folder << "\n";
        return 1;
    }

    // Load flat field once — reused for every object frame.
    cv::Mat flat32;
    if (!load_flat_as_float32(flat_path, &flat32)) {
        return 1;
    }

    // Denominator = flat + epsilon (pixel-wise).
    cv::Mat denom;
    cv::add(flat32, cv::Scalar(epsilon), denom, cv::noArray(), CV_32F);

    const std::vector<fs::path> objects = list_tiffs_sorted(in_folder);
    if (objects.empty()) {
        std::cerr << "Error: no .tif / .tiff files found in: " << in_folder << "\n";
        return 1;
    }

    std::cout << "Flat field: " << flat_path << "  (" << flat32.cols << " x "
              << flat32.rows << ")\n";
    std::cout << "Object frames found: " << objects.size() << "\n";

    int index = 0;
    for (const fs::path &obj_path : objects) {
        cv::Mat obj32;
        if (!load_object_as_float32(obj_path, &obj32)) {
            return 1;
        }
        if (!same_size(obj32, flat32)) {
            std::cerr << "Error: size mismatch between object and flat field.\n";
            std::cerr << "  object: " << obj_path << "  (" << obj32.cols << "x"
                      << obj32.rows << ")\n";
            std::cerr << "  flat:   (" << flat32.cols << "x" << flat32.rows << ")\n";
            return 1;
        }

        // Transmission image (float32).
        cv::Mat transmission;
        cv::divide(obj32, denom, transmission);

        // Output file name: _norm before trailing frame number, .tiff extension
        fs::path out_name = out_folder / (normalized_output_stem(obj_path) + ".tiff");
        if (!cv::imwrite(out_name.string(), transmission)) {
            std::cerr << "Error: failed to write: " << out_name << "\n";
            return 1;
        }

        ++index;
        if (index == 1 || index == static_cast<int>(objects.size()) ||
            (index % 50 == 0)) {
            std::cout << "  wrote " << index << " / " << objects.size() << "\r"
                      << std::flush;
        }
    }

    std::cout << "\nDone. Normalized images in: " << out_folder << "\n";
    return 0;
}
