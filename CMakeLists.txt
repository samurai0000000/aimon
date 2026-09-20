cmake_minimum_required(VERSION 3.16)
project(aimon VERSION 1.0.5 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Compiler warnings
if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra -pedantic -Wno-unused-parameter)
endif()

# Build metadata & version header generation
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
    ${CMAKE_CURRENT_SOURCE_DIR}/Version.hxx.in
    ${CMAKE_CURRENT_BINARY_DIR}/Version.hxx
    @ONLY
)

# Dependencies
find_package(OpenSSL REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(Threads REQUIRED)
find_package(Curses REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(MOSQUITTO REQUIRED libmosquitto)

# Source files
set(SOURCES
    src/Main.cxx
    src/PathUtils.cxx
    src/ConfigManager.cxx
    src/HistoryStore.cxx
    src/StateStore.cxx
    src/AntigravityCollector.cxx
    src/CursorCollector.cxx
    src/TaskRegistry.cxx
    src/DynamicToolRegistry.cxx
    src/TcpGateway.cxx
    src/McpServer.cxx
    src/MqttPublisher.cxx
    src/WebServer.cxx
    src/NcursesConsole.cxx
)

add_executable(aimon ${SOURCES})

target_compile_definitions(aimon PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)

target_include_directories(aimon PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/cpp-httplib
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/json/include
    ${MOSQUITTO_INCLUDE_DIRS}
    ${CURSES_INCLUDE_DIRS}
)

target_link_libraries(aimon PRIVATE
    OpenSSL::SSL
    OpenSSL::Crypto
    SQLite::SQLite3
    Threads::Threads
    ${MOSQUITTO_LIBRARIES}
    ${CURSES_LIBRARIES}
)

if(MOSQUITTO_LIBRARY_DIRS)
    target_link_directories(aimon PRIVATE ${MOSQUITTO_LIBRARY_DIRS})
endif()

# Tests
add_executable(test_gateway_timeout
    test/TestGatewayTimeout.cxx
    src/DynamicToolRegistry.cxx
    src/TcpGateway.cxx
)

target_include_directories(test_gateway_timeout PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/json/include
)

target_link_libraries(test_gateway_timeout PRIVATE
    Threads::Threads
)
