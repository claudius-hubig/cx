cmake_minimum_required(VERSION 3.10)
project(cx)

set(CMAKE_CXX_STANDARD 23)

add_executable(cx
    src/main.cpp
    src/terminal/setup_terminal.cpp
    src/terminal/screen.cpp
    src/buffer/buffer.cpp
    src/buffer/text_buffer.cpp
    src/buffer/help_buffer.cpp
    src/buffer/buffer_list_buffer.cpp
    src/buffer/buffer_manager.cpp
)

target_include_directories(cx PRIVATE src)
