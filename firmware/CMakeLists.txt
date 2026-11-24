cmake_minimum_required(VERSION 3.16)

set(PROJECT_VER "0.1.0" CACHE STRING "Nearic firmware version")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
include(cmake/setup_http.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(nearic)
