cmake_minimum_required(VERSION 3.20)
project(SneekPeek VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Tiny + fast: optimize for size, strip symbols, static runtime where possible.
if(MSVC)
    add_compile_options(/O1 /Os /W3 /EHsc /utf-8)
    # /MT = static CRT (no VC++ redist needed, single portable exe)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
else()
    add_compile_options(-Os -s -Wall -Wextra -finput-charset=UTF-8)
endif()

add_executable(SneekPeek WIN32
    src/main.cpp
    src/app_index.cpp
    src/app.rc
)

target_include_directories(SneekPeek PRIVATE src)

target_link_libraries(SneekPeek PRIVATE
    user32 shell32 gdi32 advapi32 shlwapi comctl32 comdlg32 powrprof
)

if(MINGW)
    # Static link so the exe runs on any Windows 10/11 box with no extra DLLs.
    target_link_options(SneekPeek PRIVATE -static -mwindows -municode)
endif()

# MSVC: Windows GUI subsystem, no console window.
if(MSVC)
    set_target_properties(SneekPeek PROPERTIES LINK_OPTIONS "/SUBSYSTEM:WINDOWS")
endif()

# Predictable output path so CI can upload it: build/bin/SneekPeek.exe
set_target_properties(SneekPeek PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin"
)
