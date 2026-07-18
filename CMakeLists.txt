cmake_minimum_required(VERSION 3.26)
project(VMS C)

file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/src/*.c"
    "${CMAKE_SOURCE_DIR}/src/*.h"
)

find_package(SDL3 REQUIRED)

add_executable(VMS ${SRC_FILES})
set_target_properties(VMS PROPERTIES MACOSX_BUNDLE FALSE)

target_link_libraries(VMS SDL3::SDL3)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -fsanitize=address -g -O1")