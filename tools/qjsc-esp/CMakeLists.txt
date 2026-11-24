# Host compiler for qjs_esp_bc.py, built from the same quickjs/ submodule as
# the device so the bytecode version matches. Normally qjs_esp_bc.py builds it.
cmake_minimum_required(VERSION 3.16)
project(qjsc_esp C)

set(QJS_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(../../quickjs quickjs EXCLUDE_FROM_ALL)

add_executable(qjsc-esp qjsc_esp.c)
target_link_libraries(qjsc-esp PRIVATE qjs)
