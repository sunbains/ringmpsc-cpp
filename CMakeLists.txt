cmake_minimum_required(VERSION 3.20)
project(ringmpsc_cpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(ringmpsc INTERFACE)
target_include_directories(ringmpsc INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_options(ringmpsc INTERFACE
    $<$<CONFIG:Release>:-O3 -march=native -fomit-frame-pointer>)

option(RINGMPSC_BUILD_TESTS "Build tests" ON)
if(RINGMPSC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
