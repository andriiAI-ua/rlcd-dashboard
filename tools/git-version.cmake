# Write the git revision of the source to a C header, for example:
#   #define RLCD_GIT_HASH "75cd8c4"
#   #define RLCD_GIT_DIRTY 1
# RLCD_GIT_DIRTY is 1 when the tracked files have changes that are not committed (new files do not count).
# Without git or outside a git repository, RLCD_GIT_HASH is empty.
# "idf.py build" runs this script at each build. The script writes the header only when its text changes,
# so the code that includes it builds again only after a new commit or a change of the "dirty" state.
#
# Input (-D): SOURCE_DIR (the project), GIT (the git program, can be empty), OUTPUT (the header).

set(hash "")
set(dirty 0)
if(GIT)
    execute_process(
        COMMAND "${GIT}" -C "${SOURCE_DIR}" rev-parse --short=7 HEAD
        OUTPUT_VARIABLE hash
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE result
        ERROR_QUIET)
    if(NOT result EQUAL 0)
        set(hash "")
    else()
        execute_process(
            COMMAND "${GIT}" -C "${SOURCE_DIR}" diff --quiet HEAD --
            RESULT_VARIABLE result
            OUTPUT_QUIET
            ERROR_QUIET)
        if(result EQUAL 1)
            set(dirty 1)
        endif()
    endif()
endif()

set(text "// Made by tools/git-version.cmake at the build. Do not edit.\n")
string(APPEND text "#define RLCD_GIT_HASH \"${hash}\"\n")
string(APPEND text "#define RLCD_GIT_DIRTY ${dirty}\n")

set(old "")
if(EXISTS "${OUTPUT}")
    file(READ "${OUTPUT}" old)
endif()
if(NOT old STREQUAL text)
    file(WRITE "${OUTPUT}" "${text}")
endif()
