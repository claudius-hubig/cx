cmake_minimum_required(VERSION 3.10)
project(cx)

set(CMAKE_CXX_STANDARD 23)

# Build everything optimized. The shared editor layer is compiled
# exactly once into the cx_core static library below (see note), so
# -O2 costs little build time while keeping the bench_* targets — and
# the CLAUDE.md performance gate — meaningful.
add_compile_options(-O2)

# Per-mode syntax tokenizers, listed once.
set(SYNTAX_SOURCES
    src/buffer/modes/c/syntax.cpp
    src/buffer/modes/dif/syntax.cpp
    src/buffer/modes/md/syntax.cpp
    src/buffer/modes/jsn/syntax.cpp
    src/buffer/modes/sh/syntax.cpp
    src/buffer/modes/yml/syntax.cpp
    src/buffer/modes/py/syntax.cpp
)

# The shared editor layer. Previously every executable listed these
# sources directly, so each translation unit was recompiled once per
# target (~20x for buffer.cpp / utf8.cpp / the syntax tokenizers).
# Compiling them once into a static library and linking that into
# every target removes ~200 redundant compilations and the parallel
# build is no longer able to fork hundreds of c++ processes. A static
# archive only contributes the object members a target actually
# references, so small tests link just what they use.
add_library(cx_core STATIC
    src/utf8.cpp
    ${SYNTAX_SOURCES}
    src/shortcut.cpp
    src/buffer/buffer.cpp
    src/buffer/buffer_view.cpp
    src/buffer/text_buffer.cpp
    src/buffer/version_history.cpp
    src/buffer/help_buffer.cpp
    src/buffer/buffer_list_buffer.cpp
    src/buffer/paste_ring_buffer.cpp
    src/buffer/dir_buffer.cpp
    src/buffer/buffer_manager.cpp
    src/terminal/setup_terminal.cpp
    src/terminal/screen.cpp
    src/terminal/panel.cpp
    src/terminal/panel_layout.cpp
)
# PUBLIC so every consumer inherits the include roots: `src` for
# `#include "buffer/..."` etc., `.` for `#include "tests/..."`.
target_include_directories(cx_core PUBLIC src .)

add_executable(cx
    src/main.cpp
    src/clipboard.cpp
    src/error_log.cpp
)
target_link_libraries(cx cx_core)

# Statically link the C++ and GCC runtimes into cx so the binary is portable
# across systems with different libstdc++ versions. glibc itself is left
# dynamic on purpose: a fully -static binary on Linux breaks NSS-based
# lookups (getpwuid, DNS, etc.) and is generally discouraged.
target_link_options(cx PRIVATE -static-libstdc++ -static-libgcc)

# --- Tests ---
# Boost.Test is the project's only external dependency, and only the
# test targets need it (cx, cx_core and the bench_* targets do not).
# Some machines (e.g. GCC 15 without installable Boost) can build a
# perfectly good cx but cannot satisfy Boost.Test, so the test section
# is optional:
#
#   * CX_BUILD_TESTS=OFF              -> tests skipped entirely.
#   * CX_BUILD_TESTS=ON (default) and
#       - Boost.Test found            -> tests built.
#       - Boost.Test missing          -> tests skipped with a notice;
#                                        cx still builds normally.
#
# find_package is QUIET (not REQUIRED) so a missing Boost downgrades
# gracefully instead of failing configuration.
option(CX_BUILD_TESTS "Build the test suite (requires Boost.Test)" ON)

if(CX_BUILD_TESTS)
    find_package(Boost QUIET COMPONENTS unit_test_framework)
endif()

if(CX_BUILD_TESTS AND Boost_FOUND)
    enable_testing()

    # Every test links cx_core; the linker pulls only the object
    # members it references. Helper keeps the per-target boilerplate
    # to one line.
    function(cx_test name)
        add_executable(test_${name} tests/test_${name}.cpp)
        target_link_libraries(test_${name} cx_core Boost::unit_test_framework)
        add_test(NAME ${name} COMMAND test_${name})
    endfunction()

    cx_test(char_width)
    cx_test(buffer)
    cx_test(text_buffer)
    cx_test(help_buffer)
    cx_test(buffer_list_buffer)
    cx_test(buffer_manager)
    cx_test(render)
    cx_test(movement)
    cx_test(comment_toggle)
    cx_test(chunk_delete)
    cx_test(scroll)
    cx_test(shortcut)
    cx_test(prompt_history)
    cx_test(syntax)
    cx_test(dir_buffer)
    cx_test(buffer_view)
    cx_test(paste_ring)
    cx_test(version_history)
    cx_test(paste_ring_buffer)
    cx_test(search)
    cx_test(file_arg)
    cx_test(mouse)
    cx_test(replace)
    cx_test(regex_search)
    cx_test(panel_layout)
    cx_test(bookmarks)

    # test_e2e drives a real PTY, so it also needs libutil.
    add_executable(test_e2e tests/test_e2e.cpp)
    target_link_libraries(test_e2e cx_core Boost::unit_test_framework util)
    add_test(NAME e2e COMMAND test_e2e)

    # test_clipboard compiles clipboard.cpp/error_log.cpp itself with a
    # macro that redirects xclip/xsel discovery to a fake bin dir, so
    # it does not use cx_core's (macro-free) clipboard objects.
    add_executable(test_clipboard
        tests/test_clipboard.cpp
        src/clipboard.cpp
        src/error_log.cpp
    )
    target_include_directories(test_clipboard PRIVATE src)
    target_compile_definitions(test_clipboard PRIVATE
        CX_FAKE_CLIPBOARD_BIN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fake_clipboard")
    target_link_libraries(test_clipboard Boost::unit_test_framework)
    add_test(NAME clipboard COMMAND test_clipboard)
elseif(CX_BUILD_TESTS AND NOT Boost_FOUND)
    message(STATUS
        "cx: Boost.Test not found — skipping test targets; cx itself "
        "still builds. Install Boost.Test to enable tests, or pass "
        "-DCX_BUILD_TESTS=OFF to silence this notice.")
endif()

# --- Documentation ---
# `doxygen` is an optional, developer-only tool: configuration succeeds
# without it and only the `docs` target is skipped. When present, the
# target regenerates HTML API docs from the sources plus README.md and
# the markdown under docs/ into ${CMAKE_BINARY_DIR}/docs/html.
find_package(Doxygen QUIET)
if(DOXYGEN_FOUND)
    set(DOXYGEN_INPUT_DIR ${CMAKE_CURRENT_SOURCE_DIR})
    set(DOXYGEN_OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/docs)
    set(DOXYFILE ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile)
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/docs/Doxyfile.in ${DOXYFILE} @ONLY)
    add_custom_target(docs
        COMMAND ${DOXYGEN_EXECUTABLE} ${DOXYFILE}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Generating API documentation with Doxygen"
        VERBATIM)
else()
    message(STATUS
        "cx: Doxygen not found — the `docs` target is unavailable. "
        "Install Doxygen to generate HTML API documentation.")
endif()

# --- Benchmarks ---
foreach(bench history editing open)
    add_executable(bench_${bench} tests/bench_${bench}.cpp)
    target_link_libraries(bench_${bench} cx_core)
endforeach()
# bench_history measures version-history latency under a real PTY-free
# harness but links libutil for the shared timing helpers.
target_link_libraries(bench_history util)
