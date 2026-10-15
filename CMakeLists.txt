# ESP Drums - ESP32-S3 USB-MIDI electronic drum module
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Only build the components that `main` actually depends on (faster builds).
idf_build_set_property(MINIMAL_BUILD ON)
project(esp_drums)
