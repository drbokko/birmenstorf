# EIGER REST demo tools (same layout as former dectris_detector_control Makefile).

set(DECTRIS_TOP_SRC "${CMAKE_SOURCE_DIR}/src")

add_library(eiger_client STATIC
    eiger_client.cpp
)
target_include_directories(eiger_client PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_compile_features(eiger_client PRIVATE cxx_std_17)

if(WIN32)
    target_link_libraries(eiger_client PRIVATE winhttp)
else()
    find_package(CURL)
    if(CURL_FOUND)
        target_compile_definitions(eiger_client PRIVATE USE_LIBCURL=1)
        target_link_libraries(eiger_client PRIVATE CURL::libcurl)
    else()
        message(WARNING
            "libcurl not found: eiger_client will build but HTTP calls fail until "
            "libcurl is installed (e.g. Debian/Ubuntu: sudo apt install libcurl4-openssl-dev).")
    endif()
endif()

function(dectris_add_eiger_demo _name)
    add_executable(${_name} "${DECTRIS_TOP_SRC}/${_name}.cpp")
    target_link_libraries(${_name} PRIVATE eiger_client)
    target_include_directories(${_name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_features(${_name} PRIVATE cxx_std_17)
endfunction()

dectris_add_eiger_demo(connect_and_configure_and_arm_detector)
dectris_add_eiger_demo(send_software_trigger)
dectris_add_eiger_demo(wait_idle_and_disarm_detector)
dectris_add_eiger_demo(disarm_detector)
