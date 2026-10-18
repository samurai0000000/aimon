##
## CMakeLists.txt (tools/eda-bridge)
##
## Copyright (C) 2026, Charles Chiou
##

cmake_minimum_required(VERSION 3.16)
project(aimon-eda-bridge VERSION 1.1.9 LANGUAGES CXX)

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

set(WHOAMI_OUTPUT)
execute_process(
    COMMAND whoami
    OUTPUT_VARIABLE WHOAMI_OUTPUT
    RESULT_VARIABLE WHOAMI_RESULT
    ERROR_VARIABLE WHOAMI_ERROR
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

set(HOSTNAME_OUTPUT)
execute_process(
    COMMAND hostname
    OUTPUT_VARIABLE HOSTNAME_OUTPUT
    RESULT_VARIABLE HOSTNAME_RESULT
    ERROR_VARIABLE HOSTNAME_ERROR
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

set(DATE_OUTPUT)
execute_process(
    COMMAND date "+%Y-%m-%d %H:%M:%S"
    OUTPUT_VARIABLE DATE_OUTPUT
    RESULT_VARIABLE DATE_RESULT
    ERROR_VARIABLE DATE_ERROR
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

configure_file(
    ${AIMON_ROOT_DIR}/Version.hxx.in
    ${CMAKE_CURRENT_BINARY_DIR}/Version.hxx
    @ONLY
)

option(AIMON_EDA_BRIDGE_WITH_QT "Build aimon Windows System Tray GUI with Qt6" OFF)

set(EDA_BRIDGE_SOURCES
    src/Main.cxx
    src/ProcessLauncher.cxx
    src/ToolDiscovery.cxx
    src/EdaRpcServer.cxx
    src/EdaBridgeAppConfig.cxx
    ${AIMON_ROOT_DIR}/src/EdaAccelerators.cxx
    ${AIMON_ROOT_DIR}/src/EasyEdaConverter.cxx
    ${AIMON_ROOT_DIR}/src/KiCadSchematicEditor.cxx
)

if(AIMON_EDA_BRIDGE_WITH_QT)
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/FetchPrebuiltQtWindows.cmake")
    list(APPEND EDA_BRIDGE_SOURCES src/EdaTrayWindow.cxx)
endif()

if(WIN32)
    enable_language(RC)
    set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/src/aimon.rc PROPERTIES
        OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/aimon.manifest;${CMAKE_CURRENT_BINARY_DIR}/Version.hxx;${CMAKE_CURRENT_SOURCE_DIR}/src/aimon.ico"
    )
    list(APPEND EDA_BRIDGE_SOURCES src/aimon.rc)
endif()

add_executable(aimon-eda-bridge ${EDA_BRIDGE_SOURCES})

target_include_directories(aimon-eda-bridge PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${AIMON_ROOT_DIR}/include
    ${AIMON_ROOT_DIR}/third_party/json/include
)

target_link_libraries(aimon-eda-bridge PRIVATE
    Threads::Threads
)

if(AIMON_EDA_BRIDGE_WITH_QT)
    target_compile_definitions(aimon-eda-bridge PRIVATE AIMON_EDA_BRIDGE_WITH_QT)
    target_include_directories(aimon-eda-bridge SYSTEM PRIVATE ${AIMON_QT_INCLUDE_DIRS})
    target_link_libraries(aimon-eda-bridge PRIVATE ${AIMON_QT_LIBRARIES})
    if(MINGW)
        target_compile_definitions(aimon-eda-bridge PRIVATE
            _GLIBCXX_USE_INIT_PRIORITY_ATTRIBUTE=0
        )
        target_link_options(aimon-eda-bridge PRIVATE -mwindows -Wl,-s)
    elseif(MSVC)
        set_target_properties(aimon-eda-bridge PROPERTIES WIN32_EXECUTABLE TRUE)
    endif()
endif()

if(WIN32)
    target_link_libraries(aimon-eda-bridge PRIVATE ws2_32 advapi32 user32 shell32 ole32 oleaut32 uuid)

    add_executable(aimon-installer-stub
        src/WindowsInstallerMain.cxx
        src/aimon.rc
    )
    target_include_directories(aimon-installer-stub PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    if(MINGW)
        target_compile_definitions(aimon-installer-stub PRIVATE
            _GLIBCXX_USE_INIT_PRIORITY_ATTRIBUTE=0
        )
        target_link_options(aimon-installer-stub PRIVATE
            -mwindows -static -static-libgcc -static-libstdc++ -Wl,-s
        )
    elseif(MSVC)
        set_target_properties(aimon-installer-stub PROPERTIES WIN32_EXECUTABLE TRUE)
    endif()
    target_link_libraries(aimon-installer-stub PRIVATE
        ole32 shell32 uuid advapi32 user32
    )
endif()
