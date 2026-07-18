cmake_minimum_required(VERSION 3.20)
project(bitforge CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32)
  message(FATAL_ERROR "bitforge targets Windows (Win32 APIs + GDI).")
endif()

add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
include_directories(${CMAKE_SOURCE_DIR}/core)

set(CORE core/process_source.cpp core/scanner.cpp core/disk_source.cpp)

# Console harness that proves the engine (IByteSource + bit_span + scanner).
add_executable(bitforge_cli cli/bitforge_cli.cpp ${CORE})

# Safe, self-owned target to scan/edit against.
add_executable(target_toy target/target_toy.cpp)

# Win32 GDI bit-grid viewer/editor. WIN32 -> /SUBSYSTEM:WINDOWS + WinMain.
add_executable(bitforge_gui WIN32 gui/bitforge_gui.cpp ${CORE})
target_link_libraries(bitforge_gui PRIVATE user32 gdi32 comdlg32)

# Build all three with:  cmake -B build && cmake --build build --config Release
