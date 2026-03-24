cmake_minimum_required(VERSION 3.20)
project(cx VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)

FetchContent_Declare(
    CLI11
    GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
    GIT_TAG        v2.4.2
)
FetchContent_MakeAvailable(CLI11)

# RE2 — use a tag that doesn't require Abseil
set(RE2_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS   OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    re2
    GIT_REPOSITORY https://github.com/google/re2.git
    GIT_TAG        2022-12-01
)
FetchContent_MakeAvailable(re2)

add_executable(cx
    src/main.cpp
    src/walker.cpp
    src/searcher.cpp
    src/simd_search.cpp
    src/index.cpp
    src/threadpool.cpp
)
target_include_directories(cx PRIVATE include/)

# Mark RE2 headers as system includes so their warnings don't fire under -Wpedantic
get_target_property(_re2_inc re2::re2 INTERFACE_INCLUDE_DIRECTORIES)
if(_re2_inc)
    target_include_directories(cx SYSTEM PRIVATE ${_re2_inc})
endif()

target_link_libraries(cx PRIVATE CLI11::CLI11 re2::re2)
target_compile_options(cx PRIVATE -Wall -Wextra -Wpedantic)

# SIMD acceleration for literal search
target_compile_options(cx PRIVATE -msse2)
include(CheckCXXCompilerFlag)
check_cxx_compiler_flag("-mavx2" COMPILER_SUPPORTS_AVX2)
if(COMPILER_SUPPORTS_AVX2)
    target_compile_options(cx PRIVATE -mavx2)
endif()
