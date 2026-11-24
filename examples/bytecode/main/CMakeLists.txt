idf_component_register(SRCS "main.c" EMBED_TXTFILES "hello.js" PRIV_REQUIRES esp-quickjs-ng)

# hello.js compiled on the host, the way an app would ship it
idf_build_get_property(python PYTHON)
set(tool "${CMAKE_CURRENT_LIST_DIR}/../../../tools/qjs_esp_bc.py")
set(qbc "${CMAKE_CURRENT_BINARY_DIR}/hello.qbc")
add_custom_command(OUTPUT "${qbc}"
    COMMAND "${python}" "${tool}" compile "${CMAKE_CURRENT_LIST_DIR}/hello.js" -o "${qbc}"
    DEPENDS "${CMAKE_CURRENT_LIST_DIR}/hello.js" "${tool}"
    VERBATIM)
add_custom_target(hello_qbc DEPENDS "${qbc}")
add_dependencies(${COMPONENT_LIB} hello_qbc)
target_add_binary_data(${COMPONENT_LIB} "${qbc}" BINARY)
