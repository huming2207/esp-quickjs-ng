# quickjs-libc.c, adjusted for ESP-IDF, written to the build dir so the
# quickjs/ submodule stays pristine.
#
#   - ESP-IDF is treated like WASI everywhere: upstream's __wasi__ gates already
#     drop what we can't do (dlopen, popen/urlGet/tmpfile, tty, fork/exec/
#     waitpid/pipe/dup/kill, symlink/readlink/lstat, realpath, mkdtemp, cputime)
#   - os.platform says "esp-idf", not "wasi"
#   - no os.Worker: its waker needs pipe() and it asks for a 2 MB pthread stack
#
# Plain string replacement so it works without patch/git on the host. Each
# anchor must match, or the build stops: re-check after bumping quickjs.

function(_qjs_replace_once var from to)
    string(FIND "${${var}}" "${from}" first)
    string(FIND "${${var}}" "${from}" last REVERSE)
    if(first EQUAL -1 OR NOT first EQUAL last)
        message(FATAL_ERROR "quickjs-libc.c: anchor not found exactly once, upstream changed?\n${from}")
    endif()
    string(REPLACE "${from}" "${to}" ${var} "${${var}}")
    set(${var} "${${var}}" PARENT_SCOPE)
endfunction()

function(qjs_esp_gen_libc src out)
    file(READ "${src}" c)

    string(FIND "${c}" "defined(__wasi__)" pos)
    if(pos EQUAL -1)
        message(FATAL_ERROR "quickjs-libc.c: no __wasi__ gates left, upstream changed?")
    endif()
    string(REPLACE "defined(__wasi__)" "(defined(__wasi__) || defined(ESP_PLATFORM))" c "${c}")

    _qjs_replace_once(c "#define OS_PLATFORM \"wasi\""
        "#ifdef ESP_PLATFORM\n#define OS_PLATFORM \"esp-idf\"\n#else\n#define OS_PLATFORM \"wasi\"\n#endif")
    _qjs_replace_once(c "#if JS_HAVE_THREADS\n#include \"quickjs-c-atomics.h\""
        "#if JS_HAVE_THREADS && !defined(ESP_PLATFORM)\n#include \"quickjs-c-atomics.h\"")

    set(c "/* generated from ${src} by port/quickjs_libc.cmake, don't edit */\n${c}")
    if(EXISTS "${out}")
        file(READ "${out}" old)
    endif()
    if(NOT "${old}" STREQUAL "${c}")
        file(WRITE "${out}" "${c}")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${src}" "${CMAKE_CURRENT_FUNCTION_LIST_FILE}")
endfunction()
