# cx — Project Guidelines

## Overview

`cx` is a simple terminal text editor written in C++. It uses **only the C++ standard library** — no external dependencies.

See docs/architecture.md for a summary of the codebase.

## Build System

- CMake (minimum 3.10), C++23
- Build: `cmake -B build && cmake --build build`
- Binary: `./build/cx`

## Formatting

- Always run `clang-format --style=file -i` on every changed file before committing

## Constraints

- No external libraries or dependencies — stdlib only
- Target: C++23
- POSIX headers (`<termios.h>`, `<sys/ioctl.h>`, `<unistd.h>`) are a necessary exception for terminal I/O; there is no stdlib alternative

## Buffer Modes

- Mode names must be at most three letters (e.g. `txt`, `c`, `md`)
- Mode-specific code lives in `src/buffer/modes/<mode>/`

## Keyboard Shortcuts

- When adding or changing a keyboard shortcut, always update both the help text (`src/buffer/help_buffer.cpp`) and `README.md` to match
