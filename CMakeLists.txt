cmake_minimum_required(VERSION 3.24)
project(ChannelChecker VERSION 2026.11.26 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
include(CTest)
option(CC_BUILD_QT "Build the Windows Qt application" ON)

if(CC_BUILD_QT AND (NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8))
    message(FATAL_ERROR "The Qt application requires Windows x64 and MSVC. Set CC_BUILD_QT=OFF for native core tests on another platform.")
endif()

add_subdirectory(src/measurement)
if(CC_BUILD_QT)
    add_subdirectory(src)
endif()
