# Build compression into the top-level build _deps directory to keep sources clean.
add_subdirectory(
    ${CMAKE_CURRENT_SOURCE_DIR}/compression
    ${CMAKE_BINARY_DIR}/_deps/compression
    EXCLUDE_FROM_ALL)

# LibTIFF (git submodule; pinned to tag v4.5.1). Built static; tools/tests/docs off.
if(DECTRIS_USE_BUNDLED_LIBTIFF AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/libtiff/CMakeLists.txt")
    set(_dectris_saved_build_shared_libs "${BUILD_SHARED_LIBS}")
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build shared libraries" FORCE)
    set(tiff-tools OFF CACHE BOOL "libtiff: build tools" FORCE)
    set(tiff-tests OFF CACHE BOOL "libtiff: build tests" FORCE)
    set(tiff-contrib OFF CACHE BOOL "libtiff: build contrib" FORCE)
    set(tiff-docs OFF CACHE BOOL "libtiff: build docs" FORCE)
    set(tiff-install OFF CACHE BOOL "libtiff: install" FORCE)
    add_subdirectory(
        ${CMAKE_CURRENT_SOURCE_DIR}/libtiff
        ${CMAKE_BINARY_DIR}/_deps/libtiff
        EXCLUDE_FROM_ALL)
    if(_dectris_saved_build_shared_libs STREQUAL "")
        unset(BUILD_SHARED_LIBS CACHE)
    else()
        set(BUILD_SHARED_LIBS "${_dectris_saved_build_shared_libs}" CACHE BOOL "Build shared libraries" FORCE)
    endif()
    unset(_dectris_saved_build_shared_libs)
    if(TARGET tiff AND NOT TARGET TIFF::TIFF)
        add_library(TIFF::TIFF ALIAS tiff)
    endif()
endif()

add_library(tinycbor OBJECT
    tinycbor/src/cborencoder.c
    tinycbor/src/cborencoder_close_container_checked.c
    tinycbor/src/cborerrorstrings.c
    tinycbor/src/cborparser.c
    tinycbor/src/cborparser_dup_string.c
    tinycbor/src/cborpretty.c
    tinycbor/src/cborpretty_stdio.c
    tinycbor/src/cborvalidation.c
    )
