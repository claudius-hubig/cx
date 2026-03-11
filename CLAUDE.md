# cx — Project Guidelines

## Overview

`cx` is a simple terminal text editor written in C++. It uses **only the C++ standard library** — no external dependencies.

## Build System

- CMake (minimum 3.10), C++23
- Build: `cmake -B build && cmake --build build`
- Binary: `./build/cx`

## Constraints

- No external libraries or dependencies — stdlib only
- Target: C++23
- POSIX headers (`<termios.h>`, `<sys/ioctl.h>`, `<unistd.h>`) are a necessary exception for terminal I/O; there is no stdlib alternative
