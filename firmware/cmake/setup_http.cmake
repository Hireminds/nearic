set(nearic_httpd_source "$ENV{IDF_PATH}/components/esp_http_server")
set(nearic_httpd_header "${nearic_httpd_source}/include/esp_http_server.h")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${nearic_httpd_header}")
file(STRINGS "${nearic_httpd_header}" nearic_httpd_binds_interface
     REGEX "struct ifreq[ \t]+\\*[ \t]*if_name;")
if(nearic_httpd_binds_interface)
    return()
endif()

# ESP-IDF v5.5.6 has SO_BINDTODEVICE in lwIP. Use Espressif's HTTP-server change
# in a local component copy so ordinary idf.py builds can use that socket option.
find_package(Git REQUIRED)
set(nearic_sdk_copy "${CMAKE_BINARY_DIR}/nearic-sdk")
set(nearic_httpd_copy "${nearic_sdk_copy}/components/esp_http_server")
set(nearic_httpd_patch "${CMAKE_CURRENT_LIST_DIR}/esp-http-server-interface.patch")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${nearic_httpd_patch}")
file(COPY "${nearic_httpd_source}/" DESTINATION "${nearic_httpd_copy}"
     PATTERN "test_apps" EXCLUDE)

# Restore the original patch targets even on repeated CMake configuration.
foreach(nearic_httpd_file IN ITEMS include/esp_http_server.h src/httpd_main.c)
    configure_file("${nearic_httpd_source}/${nearic_httpd_file}"
                   "${nearic_httpd_copy}/${nearic_httpd_file}" COPYONLY)
endforeach()
get_filename_component(nearic_build_path "${CMAKE_BINARY_DIR}" REALPATH)
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env --unset=GIT_DIR --unset=GIT_WORK_TREE
            "GIT_CEILING_DIRECTORIES=${nearic_build_path}"
            "${GIT_EXECUTABLE}" apply "${nearic_httpd_patch}"
    WORKING_DIRECTORY "${nearic_sdk_copy}"
    RESULT_VARIABLE nearic_httpd_patch_result
    ERROR_VARIABLE nearic_httpd_patch_error
)
if(NOT nearic_httpd_patch_result STREQUAL "0")
    message(FATAL_ERROR "Could not enable HTTP interface binding: ${nearic_httpd_patch_error}")
endif()
list(APPEND EXTRA_COMPONENT_DIRS "${nearic_httpd_copy}")
