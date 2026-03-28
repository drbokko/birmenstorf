#pragma once

namespace dectris {

/* Drop LibTIFF's TIFFReadDirectoryCheckOrder noise (unsorted IFD tags are common and harmless). */
void install_libtiff_warning_filter();

} // namespace dectris
