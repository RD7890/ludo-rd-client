cmake_minimum_required(VERSION 3.22.1)
project(ludordhook)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Strip and optimize aggressively for release
set(CMAKE_C_FLAGS_RELEASE   "-O3 -fvisibility=hidden -ffunction-sections -fdata-sections")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -fvisibility=hidden -ffunction-sections -fdata-sections")
set(CMAKE_SHARED_LINKER_FLAGS "-Wl,--gc-sections -Wl,--strip-all")

add_library(ludordhook SHARED src/main.cpp)

target_include_directories(ludordhook PRIVATE src)

target_link_libraries(ludordhook
    android
    log
    dl
)
