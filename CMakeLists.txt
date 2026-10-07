cmake_minimum_required(VERSION 3.15)
project(ttdb CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

file(GLOB SOURCES CONFIGURE_DEPENDS src/*.cpp)
add_executable(ttdb ${SOURCES})
target_include_directories(ttdb PRIVATE src)

if(MSVC)
    target_compile_options(ttdb PRIVATE /W4)
else()
    target_compile_options(ttdb PRIVATE -Wall -Wextra -g)
endif()
