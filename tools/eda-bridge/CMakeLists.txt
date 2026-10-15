##
## CMakeLists.txt (tools/eda-bridge)
##
## Copyright (C) 2026, Charles Chiou
##

cmake_minimum_required(VERSION 3.16)
project(aimon-eda-bridge VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra -pedantic -Wno-unused-parameter)
endif()

find_package(Threads REQUIRED)

set(AIMON_ROOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../..")

add_executable(aimon-eda-bridge
    src/Main.cxx
    src/ProcessLauncher.cxx
    src/ToolDiscovery.cxx
    src/EdaRpcServer.cxx
    ${AIMON_ROOT_DIR}/src/EasyEdaConverter.cxx
    ${AIMON_ROOT_DIR}/src/KiCadSchematicEditor.cxx
)

target_include_directories(aimon-eda-bridge PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${AIMON_ROOT_DIR}/include
    ${AIMON_ROOT_DIR}/third_party/json/include
)

target_link_libraries(aimon-eda-bridge PRIVATE
    Threads::Threads
)

if(WIN32)
    target_link_libraries(aimon-eda-bridge PRIVATE ws2_32 advapi32)
endif()
