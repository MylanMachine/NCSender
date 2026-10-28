cmake_minimum_required(VERSION 3.21)

project(NCSender VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# All executables land in one folder so they share the deployed Qt DLLs and
# the profiles.json / transfer log that live next to the exe.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

option(NCSENDER_BUILD_TOOLS "Build nccli (headless sender), nctestpeer (simulated control) and nctestrun (test suite)" ON)
option(NCSENDER_DEPLOY_QT "Run windeployqt after building so the bin folder runs standalone" ON)

# Only Core, Widgets and SerialPort are used. Qt Network is deliberately NOT
# linked: the application makes no network connections of any kind.
find_package(Qt6 6.2 REQUIRED COMPONENTS Core Widgets SerialPort)

# Standalone build: when the Qt found above is a static build (see
# build-standalone.ps1), everything is linked into one NCSender.exe. The MSVC
# runtime is linked statically too, so the exe needs no DLLs and no Visual
# C++ redistributable, and there is nothing to deploy next to it.
get_target_property(_ncs_qt_core_type Qt6::Core TYPE)
if(_ncs_qt_core_type STREQUAL "STATIC_LIBRARY")
    set(NCSENDER_STANDALONE ON)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    set(NCSENDER_DEPLOY_QT OFF CACHE BOOL "" FORCE)
    message(STATUS "NCSender: static Qt found - building a single standalone NCSender.exe")
else()
    set(NCSENDER_STANDALONE OFF)
endif()

if(MSVC)
    add_compile_options(/W3 /permissive- /utf-8)
endif()

add_subdirectory(src/core)
add_subdirectory(src/app)

if(NCSENDER_BUILD_TOOLS)
    add_subdirectory(tools/nccli)
    add_subdirectory(tools/nctestpeer)
    add_subdirectory(tools/nctestrun)
endif()

option(NCSENDER_BUILD_TESTS "Build the nccore unit tests (ctest)" ON)
if(NCSENDER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests/unit)
endif()
