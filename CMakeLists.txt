cmake_minimum_required(VERSION 3.16)
project(AllocSight VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(allocsight src/allocsight.cpp)
target_include_directories(allocsight PRIVATE src)
target_compile_definitions(allocsight PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN)

if(MINGW)
    target_compile_options(allocsight PRIVATE -O3 -Wall -Wextra)
    target_link_options(allocsight PRIVATE -municode -static -s)
elseif(MSVC)
    target_compile_options(allocsight PRIVATE /O2 /W4 /utf-8)
endif()
