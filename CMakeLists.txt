# ESP-IDF project file. See README.md for the build steps.
cmake_minimum_required(VERSION 3.22)

set(PROJECT_VER "0.1.0")

# Add the local secrets (Wi-Fi, Home Assistant) when the file exists.
# ESP-IDF reads these defaults only when it makes a new sdkconfig.
set(SDKCONFIG_DEFAULTS "sdkconfig.defaults")
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.secrets")
    list(APPEND SDKCONFIG_DEFAULTS "sdkconfig.secrets")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(rlcd-dashboard)

# Release archive for the update over Wi-Fi: build/rlcd-dashboard-<version>.tar.
# It has manifest.json (version, size, SHA-256) and the application image. "idf.py build" makes it.
idf_build_get_property(build_dir BUILD_DIR)
idf_build_get_property(project_ver PROJECT_VER)
idf_build_get_property(idf_target IDF_TARGET)
idf_build_get_config(signed CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES)
# With signing, rlcd-dashboard.bin is the signed image, made after the unsigned one.
if(signed)
    set(fw_bin_stamp "${build_dir}/.signed_bin_timestamp")
else()
    set(fw_bin_stamp "${build_dir}/.bin_timestamp")
endif()
set(fw_archive "${build_dir}/${CMAKE_PROJECT_NAME}-${project_ver}.tar")
add_custom_command(
    OUTPUT "${fw_archive}"
    COMMAND "${CMAKE_COMMAND}"
        -D "BIN=${build_dir}/${CMAKE_PROJECT_NAME}.bin"
        -D "ARCHIVE=${fw_archive}"
        -D "PROJECT=${CMAKE_PROJECT_NAME}"
        -D "VERSION=${project_ver}"
        -D "CHIP=${idf_target}"
        -D "GIT_HEADER=${build_dir}/git/git_version.h"
        -P "${CMAKE_CURRENT_LIST_DIR}/tools/fw-archive.cmake"
    DEPENDS gen_project_binary "${fw_bin_stamp}" "${CMAKE_CURRENT_LIST_DIR}/tools/fw-archive.cmake"
    COMMENT "Making the firmware archive"
    VERBATIM)
add_custom_target(fw_archive ALL DEPENDS "${fw_archive}")

# The git revision of the source (build/git/git_version.h): the settings page and Home Assistant show it
# next to the version. The script runs at each build and changes the header only when the revision changes.
find_package(Git QUIET)
set(git_header "${build_dir}/git/git_version.h")
add_custom_target(git_version
    COMMAND "${CMAKE_COMMAND}"
        -D "SOURCE_DIR=${CMAKE_CURRENT_LIST_DIR}"
        -D "GIT=${GIT_EXECUTABLE}"
        -D "OUTPUT=${git_header}"
        -P "${CMAKE_CURRENT_LIST_DIR}/tools/git-version.cmake"
    BYPRODUCTS "${git_header}"
    COMMENT "Reading the git revision"
    VERBATIM)
idf_component_get_property(main_lib main COMPONENT_LIB)
add_dependencies(${main_lib} git_version)
target_include_directories(${main_lib} PRIVATE "${build_dir}/git")
