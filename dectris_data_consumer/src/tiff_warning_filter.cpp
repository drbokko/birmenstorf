#include "tiff_warning_filter.h"

#include <tiffio.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace dectris {
namespace {

/* LibTIFF uses TIFFErrorHandler for both TIFFSetErrorHandler and TIFFSetWarningHandler. */
TIFFErrorHandler g_prev_warning = nullptr;

extern "C" void dectris_tiff_warning_handler(const char* module, const char* fmt, va_list ap) {
    /* LibTIFF uses module "TIFFReadDirectoryCheckOrder" for unsorted-tag warnings. */
    if (module && std::strstr(module, "CheckOrder") != nullptr)
        return;
    if (g_prev_warning)
        g_prev_warning(module, fmt, ap);
    else {
        if (module)
            std::fprintf(stderr, "%s: ", module);
        std::vfprintf(stderr, fmt, ap);
    }
}

} // namespace

void install_libtiff_warning_filter() {
    g_prev_warning = TIFFSetWarningHandler(dectris_tiff_warning_handler);
}

} // namespace dectris
