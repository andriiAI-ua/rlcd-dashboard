file(GLOB FONT_SRCS "${CMAKE_CURRENT_LIST_DIR}/fonts/*.c")

idf_component_register(
    SRCS "gfx.c" ${FONT_SRCS}
    INCLUDE_DIRS "include"
)
