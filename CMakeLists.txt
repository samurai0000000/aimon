cmake_minimum_required(VERSION 3.16)
project(aimon VERSION 1.2.11 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Compiler warnings
if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra -pedantic -Wno-unused-parameter -UNDEBUG)
endif()

# Optional sanitizer build (driven by `make test SANITIZE=1`)
option(AIMON_SANITIZE "Build with AddressSanitizer and UBSan" OFF)
option(AIMON_SANITIZE_CANARY "Build the sanitizer canary test" OFF)
if(AIMON_SANITIZE AND NOT MSVC)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer
                        -fno-sanitize-recover=undefined)
    add_link_options(-fsanitize=address,undefined)
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
pkg_check_modules(LIBCONFIGPP REQUIRED libconfig++)
pkg_check_modules(CPPUTEST REQUIRED cpputest)

# Library and Server Sources
set(LIB_SOURCES
    src/PathUtils.cxx
    src/ConfigManager.cxx
    src/HistoryStore.cxx
    src/AgentTelemetryDb.cxx
    src/MobileGateway.cxx
    src/StateStore.cxx
    src/AntigravityCollector.cxx
    src/CursorCollector.cxx
    src/PriceCatalog.cxx
    src/ClaudeUsageStore.cxx
    src/ClaudeAccount.cxx
    src/ClaudeCollector.cxx
    src/TaskRegistry.cxx
    src/DynamicToolRegistry.cxx
    src/TcpGateway.cxx
    src/McpServer.cxx
    src/MqttPublisher.cxx
    src/WebServer.cxx
    src/NcursesConsole.cxx
    src/InsightEngine.cxx
    src/EdaAccelerators.cxx
    src/EasyEdaConverter.cxx
    src/KiCadSchematicEditor.cxx
    src/EdaBridge.cxx
)

add_executable(aimon src/Main.cxx ${LIB_SOURCES})

target_compile_definitions(aimon PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)

target_include_directories(aimon PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/cpp-httplib
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/json/include
    ${MOSQUITTO_INCLUDE_DIRS}
    ${CURSES_INCLUDE_DIRS}
    ${LIBCONFIGPP_INCLUDE_DIRS}
)

target_link_libraries(aimon PRIVATE
    OpenSSL::SSL
    OpenSSL::Crypto
    SQLite::SQLite3
    Threads::Threads
    ${MOSQUITTO_LIBRARIES}
    ${CURSES_LIBRARIES}
    ${LIBCONFIGPP_LIBRARIES}
)

if(MOSQUITTO_LIBRARY_DIRS)
    target_link_directories(aimon PRIVATE ${MOSQUITTO_LIBRARY_DIRS})
endif()

if(LIBCONFIGPP_LIBRARY_DIRS)
    target_link_directories(aimon PRIVATE ${LIBCONFIGPP_LIBRARY_DIRS})
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

add_executable(test_agent_telemetry_db
    test/TestAgentTelemetryDb.cxx
    src/AgentTelemetryDb.cxx
    src/PathUtils.cxx
)

target_include_directories(test_agent_telemetry_db PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/json/include
)

target_link_libraries(test_agent_telemetry_db PRIVATE
    SQLite::SQLite3
    Threads::Threads
)

find_library(RAPIDCHECK_LIBRARY rapidcheck)
if(NOT RAPIDCHECK_LIBRARY)
    message(FATAL_ERROR "RapidCheck library not found (needed by the property tests)")
endif()

add_executable(test_mobile_gateway
    test/TestMobileGateway.cxx
    ${LIB_SOURCES}
)

target_compile_definitions(test_mobile_gateway PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)

target_include_directories(test_mobile_gateway PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/cpp-httplib
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/json/include
    ${MOSQUITTO_INCLUDE_DIRS}
    ${CURSES_INCLUDE_DIRS}
    ${LIBCONFIGPP_INCLUDE_DIRS}
)

target_link_libraries(test_mobile_gateway PRIVATE
    OpenSSL::SSL
    OpenSSL::Crypto
    SQLite::SQLite3
    Threads::Threads
    ${MOSQUITTO_LIBRARIES}
    ${CURSES_LIBRARIES}
    ${LIBCONFIGPP_LIBRARIES}
    ${RAPIDCHECK_LIBRARY}
)

if(MOSQUITTO_LIBRARY_DIRS)
    target_link_directories(test_mobile_gateway PRIVATE ${MOSQUITTO_LIBRARY_DIRS})
endif()

if(LIBCONFIGPP_LIBRARY_DIRS)
    target_link_directories(test_mobile_gateway PRIVATE ${LIBCONFIGPP_LIBRARY_DIRS})
endif()

add_executable(test_web_server_api
    test/TestWebServerApi.cxx
    ${LIB_SOURCES}
)

target_compile_definitions(test_web_server_api PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)

target_include_directories(test_web_server_api PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/cpp-httplib
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/json/include
    ${MOSQUITTO_INCLUDE_DIRS}
    ${CURSES_INCLUDE_DIRS}
)

target_link_libraries(test_web_server_api PRIVATE
    OpenSSL::SSL
    OpenSSL::Crypto
    SQLite::SQLite3
    Threads::Threads
    ${MOSQUITTO_LIBRARIES}
    ${CURSES_LIBRARIES}
    ${LIBCONFIGPP_LIBRARIES}
)

if(LIBCONFIGPP_LIBRARY_DIRS)
    target_link_directories(test_web_server_api PRIVATE ${LIBCONFIGPP_LIBRARY_DIRS})
endif()

add_executable(test_cursor_collector
    test/TestCursorCollector.cxx
    ${LIB_SOURCES}
)

target_compile_definitions(test_cursor_collector PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)

target_include_directories(test_cursor_collector PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/cpp-httplib
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/json/include
    ${MOSQUITTO_INCLUDE_DIRS}
    ${CURSES_INCLUDE_DIRS}
)

target_link_libraries(test_cursor_collector PRIVATE
    OpenSSL::SSL
    OpenSSL::Crypto
    SQLite::SQLite3
    Threads::Threads
    ${MOSQUITTO_LIBRARIES}
    ${CURSES_LIBRARIES}
    ${LIBCONFIGPP_LIBRARIES}
)

if(LIBCONFIGPP_LIBRARY_DIRS)
    target_link_directories(test_cursor_collector PRIVATE ${LIBCONFIGPP_LIBRARY_DIRS})
endif()

add_executable(test_libconfig_parser
    test/TestLibconfigParser.cxx
    src/ConfigManager.cxx
    src/PathUtils.cxx
)

target_include_directories(test_libconfig_parser PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/json/include
    ${LIBCONFIGPP_INCLUDE_DIRS}
    ${CPPUTEST_INCLUDE_DIRS}
)

target_link_libraries(test_libconfig_parser PRIVATE
    ${LIBCONFIGPP_LIBRARIES}
    ${CPPUTEST_LIBRARIES}
    Threads::Threads
)

if(LIBCONFIGPP_LIBRARY_DIRS)
    target_link_directories(test_libconfig_parser PRIVATE ${LIBCONFIGPP_LIBRARY_DIRS})
endif()
if(CPPUTEST_LIBRARY_DIRS)
    target_link_directories(test_libconfig_parser PRIVATE ${CPPUTEST_LIBRARY_DIRS})
endif()

if(AIMON_SANITIZE_CANARY)
    add_executable(test_sanitizer_canary test/TestSanitizerCanary.cxx)
endif()

# Property tests need RapidCheck; the library is installed system-wide.
find_library(RAPIDCHECK_LIBRARY rapidcheck)
if(NOT RAPIDCHECK_LIBRARY)
    message(FATAL_ERROR "RapidCheck library not found (needed by the property tests)")
endif()

set(AIMON_CPPUTEST_THREADSAFE_INIT "${CMAKE_CURRENT_BINARY_DIR}/cpputest_threadsafe_init.cxx")
file(WRITE "${AIMON_CPPUTEST_THREADSAFE_INIT}"
"#include <CppUTest/TestHarness.h>\n"
"#include <CppUTest/MemoryLeakWarningPlugin.h>\n"
"namespace {\n"
"__attribute__((constructor(101))) void aimonInitThreadSafeCppUTest() {\n"
"    MemoryLeakWarningPlugin::turnOnThreadSafeNewDeleteOverloads();\n"
"}\n"
"}\n")

# Helper for the Claude usage test binaries: CppUTest or RapidCheck based, all
# linked against OpenSSL/SQLite/threads and the sources they exercise.
function(aimon_add_claude_test name)
    cmake_parse_arguments(T "RAPIDCHECK;PLAIN;LIBCONFIG" "" "SOURCES" ${ARGN})
    if(NOT T_PLAIN AND NOT T_RAPIDCHECK)
        list(APPEND T_SOURCES "${AIMON_CPPUTEST_THREADSAFE_INIT}")
    endif()
    add_executable(${name} ${T_SOURCES})
    target_compile_definitions(${name} PRIVATE
        CPPHTTPLIB_OPENSSL_SUPPORT
        AIMON_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_include_directories(${name} PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/cpp-httplib
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/json/include
        ${CPPUTEST_INCLUDE_DIRS})
    target_link_libraries(${name} PRIVATE
        OpenSSL::SSL
        OpenSSL::Crypto
        SQLite::SQLite3
        Threads::Threads)
    if(T_LIBCONFIG)
        target_include_directories(${name} PRIVATE ${LIBCONFIGPP_INCLUDE_DIRS})
        target_link_libraries(${name} PRIVATE ${LIBCONFIGPP_LIBRARIES})
        if(LIBCONFIGPP_LIBRARY_DIRS)
            target_link_directories(${name} PRIVATE ${LIBCONFIGPP_LIBRARY_DIRS})
        endif()
    endif()
    if(T_PLAIN)
        # no test framework (ground-truth tools driven by a gate)
    elseif(T_RAPIDCHECK)
        target_link_libraries(${name} PRIVATE ${RAPIDCHECK_LIBRARY})
    else()
        target_link_libraries(${name} PRIVATE ${CPPUTEST_LIBRARIES})
        if(CPPUTEST_LIBRARY_DIRS)
            target_link_directories(${name} PRIVATE ${CPPUTEST_LIBRARY_DIRS})
        endif()
    endif()
endfunction()

aimon_add_claude_test(test_price_catalog
    SOURCES test/TestPriceCatalog.cxx src/PriceCatalog.cxx src/PathUtils.cxx)

aimon_add_claude_test(test_price_catalog_props RAPIDCHECK
    SOURCES test/TestPriceCatalogProps.cxx src/PriceCatalog.cxx src/PathUtils.cxx)

aimon_add_claude_test(test_claude_usage_store
    SOURCES test/TestClaudeUsageStore.cxx src/ClaudeUsageStore.cxx src/PriceCatalog.cxx src/PathUtils.cxx)

# Ground-truth tool, not a unit test: run by the gate with explicit arguments.
aimon_add_claude_test(test_claude_recount PLAIN
    SOURCES test/TestClaudeRecount.cxx src/ClaudeUsageStore.cxx src/PriceCatalog.cxx src/PathUtils.cxx)

aimon_add_claude_test(test_claude_account LIBCONFIG
    SOURCES test/TestClaudeAccount.cxx src/ClaudeAccount.cxx src/ConfigManager.cxx src/PathUtils.cxx)

aimon_add_claude_test(test_claude_collector LIBCONFIG
    SOURCES test/TestClaudeCollector.cxx src/ClaudeCollector.cxx src/ClaudeAccount.cxx
            src/ClaudeUsageStore.cxx src/PriceCatalog.cxx src/ConfigManager.cxx src/PathUtils.cxx)

aimon_add_claude_test(test_claude_parser_props RAPIDCHECK LIBCONFIG
    SOURCES test/TestClaudeParserProps.cxx src/ClaudeCollector.cxx src/ClaudeAccount.cxx
            src/ClaudeUsageStore.cxx src/PriceCatalog.cxx src/ConfigManager.cxx src/PathUtils.cxx)

add_executable(test_claude_surfaces
    test/TestClaudeSurfaces.cxx
    ${LIB_SOURCES}
)

target_compile_definitions(test_claude_surfaces PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)

target_include_directories(test_claude_surfaces PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/cpp-httplib
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/json/include
    ${MOSQUITTO_INCLUDE_DIRS}
    ${CURSES_INCLUDE_DIRS}
    ${LIBCONFIGPP_INCLUDE_DIRS}
    ${CPPUTEST_INCLUDE_DIRS}
)

target_link_libraries(test_claude_surfaces PRIVATE
    OpenSSL::SSL
    OpenSSL::Crypto
    SQLite::SQLite3
    Threads::Threads
    ${MOSQUITTO_LIBRARIES}
    ${CURSES_LIBRARIES}
    ${LIBCONFIGPP_LIBRARIES}
    ${CPPUTEST_LIBRARIES}
)

if(MOSQUITTO_LIBRARY_DIRS)
    target_link_directories(test_claude_surfaces PRIVATE ${MOSQUITTO_LIBRARY_DIRS})
endif()
if(LIBCONFIGPP_LIBRARY_DIRS)
    target_link_directories(test_claude_surfaces PRIVATE ${LIBCONFIGPP_LIBRARY_DIRS})
endif()
if(CPPUTEST_LIBRARY_DIRS)
    target_link_directories(test_claude_surfaces PRIVATE ${CPPUTEST_LIBRARY_DIRS})
endif()

aimon_add_claude_test(test_insight_engine LIBCONFIG
    SOURCES test/TestInsightEngine.cxx src/InsightEngine.cxx src/HistoryStore.cxx
            src/AgentTelemetryDb.cxx src/ClaudeUsageStore.cxx src/PriceCatalog.cxx
            src/ConfigManager.cxx src/PathUtils.cxx)

aimon_add_claude_test(test_insight_parser_props RAPIDCHECK LIBCONFIG
    SOURCES test/TestInsightParserProps.cxx src/InsightEngine.cxx src/HistoryStore.cxx
            src/AgentTelemetryDb.cxx src/ClaudeUsageStore.cxx src/PriceCatalog.cxx
            src/ConfigManager.cxx src/PathUtils.cxx)

add_executable(test_insight_surfaces
    test/TestInsightSurfaces.cxx
    ${LIB_SOURCES}
)

target_compile_definitions(test_insight_surfaces PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)

target_include_directories(test_insight_surfaces PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/cpp-httplib
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/json/include
    ${MOSQUITTO_INCLUDE_DIRS}
    ${CURSES_INCLUDE_DIRS}
    ${LIBCONFIGPP_INCLUDE_DIRS}
    ${CPPUTEST_INCLUDE_DIRS}
)

target_link_libraries(test_insight_surfaces PRIVATE
    OpenSSL::SSL
    OpenSSL::Crypto
    SQLite::SQLite3
    Threads::Threads
    ${MOSQUITTO_LIBRARIES}
    ${CURSES_LIBRARIES}
    ${LIBCONFIGPP_LIBRARIES}
    ${CPPUTEST_LIBRARIES}
)

if(MOSQUITTO_LIBRARY_DIRS)
    target_link_directories(test_insight_surfaces PRIVATE ${MOSQUITTO_LIBRARY_DIRS})
endif()
if(LIBCONFIGPP_LIBRARY_DIRS)
    target_link_directories(test_insight_surfaces PRIVATE ${LIBCONFIGPP_LIBRARY_DIRS})
endif()
if(CPPUTEST_LIBRARY_DIRS)
    target_link_directories(test_insight_surfaces PRIVATE ${CPPUTEST_LIBRARY_DIRS})
endif()

set(AIMON_SELFUPDATE_CMAKE
    "${CMAKE_CURRENT_SOURCE_DIR}/../selfupdate/windows/selfupdate.cmake"
    CACHE FILEPATH "Path to selfupdate/windows/selfupdate.cmake"
)
if(NOT EXISTS "${AIMON_SELFUPDATE_CMAKE}")
    message(FATAL_ERROR "selfupdate CMake module not found at ${AIMON_SELFUPDATE_CMAKE}")
endif()
include("${AIMON_SELFUPDATE_CMAKE}")

find_package(Qt6 QUIET COMPONENTS Core Gui Widgets)
if(NOT Qt6_FOUND)
    find_package(Qt5 QUIET COMPONENTS Core Gui Widgets)
endif()

add_executable(aimon-eda-bridge
    tools/eda-bridge/src/Main.cxx
    tools/eda-bridge/src/ProcessLauncher.cxx
    tools/eda-bridge/src/ToolDiscovery.cxx
    tools/eda-bridge/src/EdaRpcServer.cxx
    tools/eda-bridge/src/EdaBridgeAppConfig.cxx
    tools/eda-bridge/src/EdaTrayWindow.cxx
    src/EdaAccelerators.cxx
    src/EasyEdaConverter.cxx
    src/KiCadSchematicEditor.cxx
)

selfupdate_target_sources(aimon-eda-bridge)

if(Qt6_FOUND)
    target_compile_definitions(aimon-eda-bridge PRIVATE AIMON_EDA_BRIDGE_WITH_QT=1)
    target_link_libraries(aimon-eda-bridge PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets)
elseif(Qt5_FOUND)
    target_compile_definitions(aimon-eda-bridge PRIVATE AIMON_EDA_BRIDGE_WITH_QT=1)
    target_link_libraries(aimon-eda-bridge PRIVATE Qt5::Core Qt5::Gui Qt5::Widgets)
endif()

target_include_directories(aimon-eda-bridge PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/eda-bridge/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/json/include
)

target_link_libraries(aimon-eda-bridge PRIVATE
    Threads::Threads
)

aimon_add_claude_test(test_eda_bridge LIBCONFIG
    SOURCES test/TestEdaBridge.cxx src/EdaBridge.cxx src/EdaAccelerators.cxx
            src/EasyEdaConverter.cxx src/KiCadSchematicEditor.cxx
            src/DynamicToolRegistry.cxx src/TcpGateway.cxx
            src/ConfigManager.cxx src/PathUtils.cxx)
target_link_libraries(test_eda_bridge PRIVATE ${RAPIDCHECK_LIBRARY})

aimon_add_claude_test(test_eda_bridge_props RAPIDCHECK LIBCONFIG
    SOURCES test/TestEdaBridgeProps.cxx src/EdaBridge.cxx src/EdaAccelerators.cxx
            src/EasyEdaConverter.cxx src/KiCadSchematicEditor.cxx
            src/DynamicToolRegistry.cxx src/TcpGateway.cxx
            src/ConfigManager.cxx src/PathUtils.cxx)

aimon_add_claude_test(test_eda_bridge_daemon LIBCONFIG
    SOURCES test/TestEdaBridgeDaemon.cxx src/EdaBridge.cxx src/EdaAccelerators.cxx
            src/EasyEdaConverter.cxx src/KiCadSchematicEditor.cxx
            tools/eda-bridge/src/ProcessLauncher.cxx
            tools/eda-bridge/src/ToolDiscovery.cxx
            tools/eda-bridge/src/EdaRpcServer.cxx
            tools/eda-bridge/src/EdaBridgeAppConfig.cxx
            tools/eda-bridge/src/EdaTrayWindow.cxx
            src/DynamicToolRegistry.cxx src/TcpGateway.cxx
            src/ConfigManager.cxx src/PathUtils.cxx)

selfupdate_target_sources(test_eda_bridge_daemon)
target_link_libraries(test_eda_bridge_daemon PRIVATE ${RAPIDCHECK_LIBRARY})

if(Qt6_FOUND)
    target_compile_definitions(test_eda_bridge_daemon PRIVATE AIMON_EDA_BRIDGE_WITH_QT=1)
    target_link_libraries(test_eda_bridge_daemon PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets)
elseif(Qt5_FOUND)
    target_compile_definitions(test_eda_bridge_daemon PRIVATE AIMON_EDA_BRIDGE_WITH_QT=1)
    target_link_libraries(test_eda_bridge_daemon PRIVATE Qt5::Core Qt5::Gui Qt5::Widgets)
endif()

target_include_directories(test_eda_bridge_daemon PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/eda-bridge/include
)

aimon_add_claude_test(test_easyeda_converter_props RAPIDCHECK LIBCONFIG
    SOURCES test/TestEasyEdaConverterProps.cxx src/EasyEdaConverter.cxx
            src/KiCadSchematicEditor.cxx src/PathUtils.cxx)

add_executable(test_eda_surfaces
    test/TestEdaSurfaces.cxx
    tools/eda-bridge/src/ProcessLauncher.cxx
    tools/eda-bridge/src/ToolDiscovery.cxx
    tools/eda-bridge/src/EdaRpcServer.cxx
    ${LIB_SOURCES}
)

target_compile_definitions(test_eda_surfaces PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)

target_include_directories(test_eda_surfaces PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/eda-bridge/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/cpp-httplib
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/json/include
    ${MOSQUITTO_INCLUDE_DIRS}
    ${CURSES_INCLUDE_DIRS}
    ${LIBCONFIGPP_INCLUDE_DIRS}
    ${CPPUTEST_INCLUDE_DIRS}
)

target_link_libraries(test_eda_surfaces PRIVATE
    OpenSSL::SSL
    OpenSSL::Crypto
    SQLite::SQLite3
    Threads::Threads
    ${MOSQUITTO_LIBRARIES}
    ${CURSES_LIBRARIES}
    ${LIBCONFIGPP_LIBRARIES}
    ${CPPUTEST_LIBRARIES}
)

if(MOSQUITTO_LIBRARY_DIRS)
    target_link_directories(test_eda_surfaces PRIVATE ${MOSQUITTO_LIBRARY_DIRS})
endif()
if(LIBCONFIGPP_LIBRARY_DIRS)
    target_link_directories(test_eda_surfaces PRIVATE ${LIBCONFIGPP_LIBRARY_DIRS})
endif()
if(CPPUTEST_LIBRARY_DIRS)
    target_link_directories(test_eda_surfaces PRIVATE ${CPPUTEST_LIBRARY_DIRS})
endif()
