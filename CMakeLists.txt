cmake_minimum_required(VERSION 3.10)
project(cx)

set(CMAKE_CXX_STANDARD 23)

add_executable(cx
    src/main.cpp
    src/error_log.cpp
    src/terminal/setup_terminal.cpp
    src/terminal/screen.cpp
    src/buffer/buffer.cpp
    src/buffer/text_buffer.cpp
    src/buffer/help_buffer.cpp
    src/buffer/buffer_list_buffer.cpp
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
    src/buffer/text_buffer.cpp
    src/buffer/help_buffer.cpp
)
target_include_directories(test_buffer PRIVATE src)
target_link_libraries(test_buffer Boost::unit_test_framework)
add_test(NAME buffer COMMAND test_buffer)

add_executable(test_text_buffer
    tests/test_text_buffer.cpp
    src/buffer/buffer.cpp
    src/buffer/text_buffer.cpp
)
target_include_directories(test_text_buffer PRIVATE src .)
target_link_libraries(test_text_buffer Boost::unit_test_framework)
add_test(NAME text_buffer COMMAND test_text_buffer)

add_executable(test_help_buffer
    tests/test_help_buffer.cpp
    src/buffer/buffer.cpp
    src/buffer/help_buffer.cpp
)
target_include_directories(test_help_buffer PRIVATE src)
target_link_libraries(test_help_buffer Boost::unit_test_framework)
add_test(NAME help_buffer COMMAND test_help_buffer)

add_executable(test_buffer_list_buffer
    tests/test_buffer_list_buffer.cpp
    src/buffer/buffer.cpp
    src/buffer/buffer_list_buffer.cpp
)
target_include_directories(test_buffer_list_buffer PRIVATE src)
target_link_libraries(test_buffer_list_buffer Boost::unit_test_framework)
add_test(NAME buffer_list_buffer COMMAND test_buffer_list_buffer)

add_executable(test_buffer_manager
    tests/test_buffer_manager.cpp
    src/buffer/buffer.cpp
    src/buffer/text_buffer.cpp
    src/buffer/help_buffer.cpp
    src/buffer/buffer_list_buffer.cpp
    src/buffer/buffer_manager.cpp
)
target_include_directories(test_buffer_manager PRIVATE src .)
target_link_libraries(test_buffer_manager Boost::unit_test_framework)
add_test(NAME buffer_manager COMMAND test_buffer_manager)
