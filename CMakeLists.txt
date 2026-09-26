cmake_minimum_required(VERSION 3.16)
project(aimon VERSION 1.0.9 LANGUAGES CXX)

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
    src/TaskRegistry.cxx
    src/DynamicToolRegistry.cxx
    src/TcpGateway.cxx
    src/McpServer.cxx
    src/MqttPublisher.cxx
    src/WebServer.cxx
    src/NcursesConsole.cxx
    src/ProcessMonitor.cxx
    src/ServiceSupervisor.cxx
    src/GeminiTriage.cxx
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

add_executable(test_mobile_gateway
    test/TestMobileGateway.cxx
    src/MobileGateway.cxx
    src/AgentTelemetryDb.cxx
    src/PathUtils.cxx
)

target_include_directories(test_mobile_gateway PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/json/include
)

target_link_libraries(test_mobile_gateway PRIVATE
    SQLite::SQLite3
    Threads::Threads
)

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

add_executable(test_process_monitor_lifecycle
    test/TestProcessMonitorLifecycle.cxx
    src/ProcessMonitor.cxx
    src/ConfigManager.cxx
    src/PathUtils.cxx
)

target_include_directories(test_process_monitor_lifecycle PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/json/include
    ${LIBCONFIGPP_INCLUDE_DIRS}
    ${CPPUTEST_INCLUDE_DIRS}
)

target_link_libraries(test_process_monitor_lifecycle PRIVATE
    ${LIBCONFIGPP_LIBRARIES}
    ${CPPUTEST_LIBRARIES}
    Threads::Threads
)

if(LIBCONFIGPP_LIBRARY_DIRS)
    target_link_directories(test_process_monitor_lifecycle PRIVATE ${LIBCONFIGPP_LIBRARY_DIRS})
endif()
if(CPPUTEST_LIBRARY_DIRS)
    target_link_directories(test_process_monitor_lifecycle PRIVATE ${CPPUTEST_LIBRARY_DIRS})
endif()

add_executable(test_circuit_breaker_quarantine
    test/TestCircuitBreakerQuarantine.cxx
    src/ProcessMonitor.cxx
    src/ConfigManager.cxx
    src/PathUtils.cxx
)

target_include_directories(test_circuit_breaker_quarantine PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/json/include
    ${LIBCONFIGPP_INCLUDE_DIRS}
    ${CPPUTEST_INCLUDE_DIRS}
)

target_link_libraries(test_circuit_breaker_quarantine PRIVATE
    ${LIBCONFIGPP_LIBRARIES}
    ${CPPUTEST_LIBRARIES}
    Threads::Threads
)

if(LIBCONFIGPP_LIBRARY_DIRS)
    target_link_directories(test_circuit_breaker_quarantine PRIVATE ${LIBCONFIGPP_LIBRARY_DIRS})
endif()
if(CPPUTEST_LIBRARY_DIRS)
    target_link_directories(test_circuit_breaker_quarantine PRIVATE ${CPPUTEST_LIBRARY_DIRS})
endif()

add_executable(test_service_supervisor
    test/TestServiceSupervisor.cxx
    src/ServiceSupervisor.cxx
    src/ConfigManager.cxx
    src/PathUtils.cxx
)

target_include_directories(test_service_supervisor PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/json/include
    ${LIBCONFIGPP_INCLUDE_DIRS}
    ${CPPUTEST_INCLUDE_DIRS}
)

target_link_libraries(test_service_supervisor PRIVATE
    ${LIBCONFIGPP_LIBRARIES}
    ${CPPUTEST_LIBRARIES}
    Threads::Threads
)

if(LIBCONFIGPP_LIBRARY_DIRS)
    target_link_directories(test_service_supervisor PRIVATE ${LIBCONFIGPP_LIBRARY_DIRS})
endif()
if(CPPUTEST_LIBRARY_DIRS)
    target_link_directories(test_service_supervisor PRIVATE ${CPPUTEST_LIBRARY_DIRS})
endif()

add_executable(test_gemini_triage
    test/TestGeminiTriage.cxx
    src/GeminiTriage.cxx
    src/ConfigManager.cxx
    src/PathUtils.cxx
)

target_compile_definitions(test_gemini_triage PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)

target_include_directories(test_gemini_triage PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/cpp-httplib
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/json/include
    ${LIBCONFIGPP_INCLUDE_DIRS}
    ${CPPUTEST_INCLUDE_DIRS}
)

target_link_libraries(test_gemini_triage PRIVATE
    OpenSSL::SSL
    OpenSSL::Crypto
    ${LIBCONFIGPP_LIBRARIES}
    ${CPPUTEST_LIBRARIES}
    Threads::Threads
)

if(LIBCONFIGPP_LIBRARY_DIRS)
    target_link_directories(test_gemini_triage PRIVATE ${LIBCONFIGPP_LIBRARY_DIRS})
endif()
if(CPPUTEST_LIBRARY_DIRS)
    target_link_directories(test_gemini_triage PRIVATE ${CPPUTEST_LIBRARY_DIRS})
endif()

add_executable(test_supervisor_api_and_mcp
    test/TestSupervisorApiAndMcp.cxx
    ${LIB_SOURCES}
)

target_compile_definitions(test_supervisor_api_and_mcp PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)

target_include_directories(test_supervisor_api_and_mcp PRIVATE
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

target_link_libraries(test_supervisor_api_and_mcp PRIVATE
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
    target_link_directories(test_supervisor_api_and_mcp PRIVATE ${MOSQUITTO_LIBRARY_DIRS})
endif()
if(LIBCONFIGPP_LIBRARY_DIRS)
    target_link_directories(test_supervisor_api_and_mcp PRIVATE ${LIBCONFIGPP_LIBRARY_DIRS})
endif()
if(CPPUTEST_LIBRARY_DIRS)
    target_link_directories(test_supervisor_api_and_mcp PRIVATE ${CPPUTEST_LIBRARY_DIRS})
endif()

add_executable(test_fleet_integration
    test/TestFleetIntegration.cxx
    src/ServiceSupervisor.cxx
    src/ProcessMonitor.cxx
    src/ConfigManager.cxx
    src/PathUtils.cxx
)

target_include_directories(test_fleet_integration PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/json/include
    ${LIBCONFIGPP_INCLUDE_DIRS}
    ${CPPUTEST_INCLUDE_DIRS}
)

target_link_libraries(test_fleet_integration PRIVATE
    ${LIBCONFIGPP_LIBRARIES}
    ${CPPUTEST_LIBRARIES}
    Threads::Threads
)

if(LIBCONFIGPP_LIBRARY_DIRS)
    target_link_directories(test_fleet_integration PRIVATE ${LIBCONFIGPP_LIBRARY_DIRS})
endif()
if(CPPUTEST_LIBRARY_DIRS)
    target_link_directories(test_fleet_integration PRIVATE ${CPPUTEST_LIBRARY_DIRS})
endif()


