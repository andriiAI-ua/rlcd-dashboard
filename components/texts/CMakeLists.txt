# The texts of the screen, the page and Home Assistant come from the language files lang/*.json.
# tools/lang.py makes the C tables from them when CMake runs; a change of a language file runs CMake again.
set(lang_dir "${CMAKE_CURRENT_LIST_DIR}/../../lang")
set(lang_script "${CMAKE_CURRENT_LIST_DIR}/../../tools/lang.py")
set(gen_dir "${CMAKE_CURRENT_BINARY_DIR}/gen")

if(CMAKE_BUILD_EARLY_EXPANSION)
    idf_component_register(INCLUDE_DIRS "include")
    return()
endif()

file(GLOB lang_files "${lang_dir}/*.json")
idf_build_get_property(python PYTHON)
execute_process(
    COMMAND "${python}" "${lang_script}" --out "${gen_dir}" --default uk ${lang_files}
    RESULT_VARIABLE lang_result
    ERROR_VARIABLE lang_error)
if(NOT lang_result EQUAL 0)
    message(FATAL_ERROR "Language files: ${lang_error}")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${lang_files} "${lang_script}" "${lang_dir}")

idf_component_register(SRCS "texts.c" "${gen_dir}/texts_data.c"
                       INCLUDE_DIRS "include" "${gen_dir}"
                       PRIV_INCLUDE_DIRS "private")
