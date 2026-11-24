# QuickJS-NG for ESP-IDF.
# quickjs/ is the upstream submodule (untouched), port/ is libc glue,
# src/ is the FreeRTOS loop and the native modules.

set(qjs_libc "${CMAKE_CURRENT_BINARY_DIR}/quickjs-libc.c")
set(qjs_core "${CMAKE_CURRENT_BINARY_DIR}/quickjs.c")
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    include("${CMAKE_CURRENT_LIST_DIR}/port/quickjs_libc.cmake")
    qjs_esp_gen_libc("${CMAKE_CURRENT_LIST_DIR}/quickjs/quickjs-libc.c" "${qjs_libc}")
    qjs_esp_gen_core("${CMAKE_CURRENT_LIST_DIR}/quickjs/quickjs.c" "${qjs_core}")
endif()

idf_component_register(
    SRCS
        "quickjs/dtoa.c"
        "quickjs/libregexp.c"
        "quickjs/libunicode.c"
        "${qjs_core}"
        "${qjs_libc}"
        "port/libc_glue.c"
        "src/rt.c"
        "src/bytecode.c"
        "src/loop.c"
        "src/util.c"
        "src/mod_udp.c"
        "src/mod_posix_socket.c"
        "src/mod_ws.c"
    INCLUDE_DIRS "include" "quickjs"
    PRIV_INCLUDE_DIRS "port/include" "src"
    PRIV_REQUIRES vfs lwip tcp_transport esp-tls mbedtls http_parser esp_timer heap
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE _GNU_SOURCE)

# Upstream code. On the ESP toolchains int32_t is 'long' and uint32_t is
# 'unsigned long', so upstream's int* <-> int32_t* casts (JS_ToInt32(&some_int),
# string_getc(p, (int *)&u32), ...) break strict aliasing. That's not just a
# warning: at -O2 GCC drops the store and e.g. string iteration loops forever.
# -fno-strict-aliasing makes those casts safe; the warnings are then noise.
set_source_files_properties(
    quickjs/dtoa.c quickjs/libregexp.c quickjs/libunicode.c "${qjs_core}" "${qjs_libc}"
    PROPERTIES COMPILE_OPTIONS
    "-fno-strict-aliasing;-Wno-incompatible-pointer-types;-Wno-format;-Wno-implicit-fallthrough"
)

# Not in the generated file, so changing it doesn't regenerate 2 MB of C
set_property(SOURCE "${qjs_core}" APPEND PROPERTY COMPILE_DEFINITIONS
    "JS_INTERRUPT_COUNTER_INIT=${CONFIG_QJS_ESP_INTERRUPT_INTERVAL}")
