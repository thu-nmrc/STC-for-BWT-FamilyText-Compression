cmake_minimum_required(VERSION 3.16)

project("stc-v1.0" LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(stc dcv5.cpp libsais/libsais.c)

if (MSVC)
  target_compile_options(stc PRIVATE /O2 /W4)
else()
  target_compile_options(stc PRIVATE -O2 -Wall -Wextra -Wpedantic)
endif()
