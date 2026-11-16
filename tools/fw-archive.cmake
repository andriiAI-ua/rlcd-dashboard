# Make the release archive of the firmware: a tar file with manifest.json and the application image.
# The device reads the archive as a stream, so manifest.json must be the first entry.
# Run: cmake -D BIN=... -D ARCHIVE=... -D PROJECT=... -D VERSION=... -D CHIP=... [-D GIT_HEADER=...]
#      -P fw-archive.cmake
# The manifest has also:
#   "git":        the git revision from GIT_HEADER (build/git/git_version.h), with "-dirty" for uncommitted changes;
#   "built":      the local time of the build computer when it made the archive ("2026-11-14 19:34");
#   "elf_sha256": the SHA-256 of the ELF file from the application description in the image. Each build has
#                 its own value; the device compares it with its own one (esp_app_get_elf_sha256()).
cmake_minimum_required(VERSION 3.22)

foreach(var BIN ARCHIVE PROJECT VERSION CHIP)
    if(NOT DEFINED ${var} OR "${${var}}" STREQUAL "")
        message(FATAL_ERROR "fw-archive: ${var} is not set")
    endif()
endforeach()

file(SHA256 "${BIN}" sha256)
file(SIZE "${BIN}" size)
get_filename_component(bin_name "${BIN}" NAME)

# The git revision from the header that tools/git-version.cmake writes.
set(git "")
if(DEFINED GIT_HEADER AND EXISTS "${GIT_HEADER}")
    file(STRINGS "${GIT_HEADER}" hash_line REGEX "RLCD_GIT_HASH")
    file(STRINGS "${GIT_HEADER}" dirty_line REGEX "RLCD_GIT_DIRTY")
    string(REGEX MATCH "\"([0-9a-f]*)\"" _ "${hash_line}")
    set(git "${CMAKE_MATCH_1}")
    if(git AND dirty_line MATCHES "RLCD_GIT_DIRTY 1")
        string(APPEND git "-dirty")
    endif()
endif()

string(TIMESTAMP built "%Y-%m-%d %H:%M")

# The application description is at offset 32 of the image: magic word 0xABCD5432, then
# app_elf_sha256 at offset 32 + 144 = 176.
file(READ "${BIN}" desc_magic OFFSET 32 LIMIT 4 HEX)
set(elf_sha256 "")
if(desc_magic STREQUAL "3254cdab")
    file(READ "${BIN}" elf_sha256 OFFSET 176 LIMIT 32 HEX)
endif()

# Put the two files in an empty folder, so that the archive has only the file names.
set(stage "${ARCHIVE}.d")
file(REMOVE_RECURSE "${stage}")
file(MAKE_DIRECTORY "${stage}")
file(WRITE "${stage}/manifest.json"
    "{\n"
    "  \"project\": \"${PROJECT}\",\n"
    "  \"version\": \"${VERSION}\",\n"
    "  \"git\": \"${git}\",\n"
    "  \"built\": \"${built}\",\n"
    "  \"elf_sha256\": \"${elf_sha256}\",\n"
    "  \"chip\": \"${CHIP}\",\n"
    "  \"file\": \"${bin_name}\",\n"
    "  \"size\": ${size},\n"
    "  \"sha256\": \"${sha256}\"\n"
    "}\n")
file(COPY_FILE "${BIN}" "${stage}/${bin_name}")

file(REMOVE "${ARCHIVE}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E tar cf "${ARCHIVE}" --format=gnutar manifest.json "${bin_name}"
    WORKING_DIRECTORY "${stage}"
    RESULT_VARIABLE result)
file(REMOVE_RECURSE "${stage}")
if(NOT result EQUAL 0)
    message(FATAL_ERROR "fw-archive: tar failed (${result})")
endif()
message(STATUS "Firmware archive: ${ARCHIVE}")
message(STATUS "Version ${VERSION} (${git}), built ${built}, ${size} bytes, SHA-256 ${sha256}")
