cmake_minimum_required(VERSION 3.10)
project(cx)

set(CMAKE_CXX_STANDARD 23)

add_executable(cx
    src/main.cpp
    src/error_log.cpp
    src/utf8.cpp
    src/buffer/modes/c/syntax.cpp
    src/buffer/modes/md/syntax.cpp
    src/buffer/modes/jsn/syntax.cpp
    src/shortcut.cpp
    src/terminal/setup_terminal.cpp
    src/terminal/screen.cpp
    src/buffer/buffer.cpp
    src/buffer/text_buffer.cpp
    src/buffer/help_buffer.cpp
    src/buffer/buffer_list_buffer.cpp
    src/buffer/paste_ring_buffer.cpp
    src/buffer/dir_buffer.cpp
    src/buffer/buffer_manager.cpp
)

target_include_directories(cx PRIVATE src)

# --- Tests ---
enable_testing()
find_package(Boost REQUIRED COMPONENTS unit_test_framework)

add_executable(test_char_width tests/test_char_width.cpp)
target_include_directories(test_char_width PRIVATE src)
target_link_libraries(test_char_width Boost::unit_test_framework)
add_test(NAME char_width COMMAND test_char_width)

add_executable(test_buffer
    tests/test_buffer.cpp
    src/buffer/buffer.cpp
    src/utf8.cpp
    src/buffer/modes/c/syntax.cpp
    src/buffer/modes/md/syntax.cpp
    src/buffer/modes/jsn/syntax.cpp
    src/buffer/text_buffer.cpp
    src/buffer/help_buffer.cpp
    src/shortcut.cpp
)
target_include_directories(test_buffer PRIVATE src)
target_link_libraries(test_buffer Boost::unit_test_framework)
add_test(NAME buffer COMMAND test_buffer)

add_executable(test_text_buffer
    tests/test_text_buffer.cpp
    src/buffer/buffer.cpp
    src/buffer/modes/c/syntax.cpp
    src/buffer/modes/md/syntax.cpp
    src/buffer/modes/jsn/syntax.cpp
    src/buffer/text_buffer.cpp
)
target_include_directories(test_text_buffer PRIVATE src .)
target_link_libraries(test_text_buffer Boost::unit_test_framework)
add_test(NAME text_buffer COMMAND test_text_buffer)

add_executable(test_help_buffer
    tests/test_help_buffer.cpp
    src/buffer/buffer.cpp
    src/utf8.cpp
    src/buffer/modes/c/syntax.cpp
    src/buffer/modes/md/syntax.cpp
    src/buffer/modes/jsn/syntax.cpp
    src/buffer/help_buffer.cpp
    src/shortcut.cpp
)
target_include_directories(test_help_buffer PRIVATE src)
target_link_libraries(test_help_buffer Boost::unit_test_framework)
add_test(NAME help_buffer COMMAND test_help_buffer)

add_executable(test_buffer_list_buffer
    tests/test_buffer_list_buffer.cpp
    src/buffer/buffer.cpp
    src/buffer/modes/c/syntax.cpp
    src/buffer/modes/md/syntax.cpp
    src/buffer/modes/jsn/syntax.cpp
    src/buffer/buffer_list_buffer.cpp
)
target_include_directories(test_buffer_list_buffer PRIVATE src)
target_link_libraries(test_buffer_list_buffer Boost::unit_test_framework)
add_test(NAME buffer_list_buffer COMMAND test_buffer_list_buffer)

add_executable(test_buffer_manager
    tests/test_buffer_manager.cpp
    src/buffer/buffer.cpp
    src/utf8.cpp
    src/buffer/modes/c/syntax.cpp
    src/buffer/modes/md/syntax.cpp
    src/buffer/modes/jsn/syntax.cpp
    src/buffer/text_buffer.cpp
    src/buffer/help_buffer.cpp
    src/buffer/buffer_list_buffer.cpp
    src/buffer/paste_ring_buffer.cpp
    src/buffer/dir_buffer.cpp
    src/buffer/buffer_manager.cpp
    src/shortcut.cpp
)
target_include_directories(test_buffer_manager PRIVATE src .)
target_link_libraries(test_buffer_manager Boost::unit_test_framework)
add_test(NAME buffer_manager COMMAND test_buffer_manager)

add_executable(test_render
    tests/test_render.cpp
    src/buffer/buffer.cpp
    src/buffer/modes/c/syntax.cpp
    src/buffer/modes/md/syntax.cpp
    src/buffer/modes/jsn/syntax.cpp
    src/buffer/text_buffer.cpp
)
target_include_directories(test_render PRIVATE src .)
target_link_libraries(test_render Boost::unit_test_framework)
add_test(NAME render COMMAND test_render)

add_executable(test_movement
    tests/test_movement.cpp
    src/buffer/buffer.cpp
    src/buffer/modes/c/syntax.cpp
    src/buffer/modes/md/syntax.cpp
    src/buffer/modes/jsn/syntax.cpp
    src/buffer/text_buffer.cpp
)
target_include_directories(test_movement PRIVATE src .)
target_link_libraries(test_movement Boost::unit_test_framework)
add_test(NAME movement COMMAND test_movement)

add_executable(test_chunk_delete
    tests/test_chunk_delete.cpp
    src/buffer/buffer.cpp
    src/utf8.cpp
    src/buffer/modes/c/syntax.cpp
    src/buffer/modes/md/syntax.cpp
    src/buffer/modes/jsn/syntax.cpp
    src/buffer/text_buffer.cpp
    src/buffer/help_buffer.cpp
    src/shortcut.cpp
)
target_include_directories(test_chunk_delete PRIVATE src .)
target_link_libraries(test_chunk_delete Boost::unit_test_framework)
add_test(NAME chunk_delete COMMAND test_chunk_delete)

add_executable(test_scroll
    tests/test_scroll.cpp
    src/buffer/buffer.cpp
    src/buffer/modes/c/syntax.cpp
    src/buffer/modes/md/syntax.cpp
    src/buffer/modes/jsn/syntax.cpp
    src/buffer/text_buffer.cpp
)
target_include_directories(test_scroll PRIVATE src .)
target_link_libraries(test_scroll Boost::unit_test_framework)
add_test(NAME scroll COMMAND test_scroll)

add_executable(test_shortcut
    tests/test_shortcut.cpp
    src/shortcut.cpp
)
target_include_directories(test_shortcut PRIVATE src)
target_link_libraries(test_shortcut Boost::unit_test_framework)
add_test(NAME shortcut COMMAND test_shortcut)

add_executable(test_prompt_history
    tests/test_prompt_history.cpp
)
target_include_directories(test_prompt_history PRIVATE src)
target_link_libraries(test_prompt_history Boost::unit_test_framework)
add_test(NAME prompt_history COMMAND test_prompt_history)

add_executable(test_syntax
    tests/test_syntax.cpp
    src/buffer/buffer.cpp
    src/buffer/modes/c/syntax.cpp
    src/buffer/modes/md/syntax.cpp
    src/buffer/modes/jsn/syntax.cpp
    src/buffer/text_buffer.cpp
)
target_include_directories(test_syntax PRIVATE src .)
target_link_libraries(test_syntax Boost::unit_test_framework)
add_test(NAME syntax COMMAND test_syntax)

add_executable(test_dir_buffer
    tests/test_dir_buffer.cpp
    src/buffer/buffer.cpp
    src/buffer/modes/c/syntax.cpp
    src/buffer/modes/md/syntax.cpp
    src/buffer/modes/jsn/syntax.cpp
    src/buffer/dir_buffer.cpp
)
target_include_directories(test_dir_buffer PRIVATE src .)
target_link_libraries(test_dir_buffer Boost::unit_test_framework)
add_test(NAME dir_buffer COMMAND test_dir_buffer)

add_executable(test_e2e
    tests/test_e2e.cpp
    src/buffer/buffer.cpp
    src/buffer/modes/c/syntax.cpp
    src/buffer/modes/md/syntax.cpp
    src/buffer/modes/jsn/syntax.cpp
    src/buffer/text_buffer.cpp
)
target_include_directories(test_e2e PRIVATE src .)
target_link_libraries(test_e2e Boost::unit_test_framework util)
add_test(NAME e2e COMMAND test_e2e)
