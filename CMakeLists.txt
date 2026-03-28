cmake_minimum_required(VERSION 3.11...3.31)
project(birmenstorf LANGUAGES C CXX)

# Single top-level output tree: executables in ./bin, static libs in ./lib
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/lib")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/lib")

add_subdirectory(src/data_consumer)
add_subdirectory(src/detector_control)
