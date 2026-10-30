cmake_minimum_required(VERSION 3.20)

project(ThreadHaul
    VERSION 1.0.0
    DESCRIPTION "Windows service that keeps latency-critical workloads responsive under CPU pressure"
    LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "ThreadHaul uses the Win32 API and builds on Windows only.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# Static CRT so the binaries run without a separate Visual C++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
if(MSVC)
    add_compile_options(/W3 /utf-8 /permissive-)
endif()

# --------------------------------------------------------------------------
# Core library: everything except the entry points.
# --------------------------------------------------------------------------
add_library(threadhaul_core STATIC
    src/actuator/ActuatorManager.cpp
    src/actuator/PriorityControl.cpp
    src/actuator/StateMachine.cpp
    src/actuator/ThreadControl.cpp
    src/telemetry/AdaptiveThresholds.cpp
    src/telemetry/CpuMath.cpp
    src/telemetry/ProcessMonitor.cpp
    src/telemetry/ProcessSafety.cpp
    src/telemetry/ProcessTable.cpp
    src/config/Config.cpp
    src/logging/EventLogWriter.cpp
    src/logging/JsonLinesLogger.cpp
    src/service/Alerts.cpp
    src/service/AutoProtect.cpp
    src/service/ConfigSecurity.cpp
    src/service/ControlPipe.cpp
    src/service/Coordination.cpp
    src/service/Daemon.cpp
    src/service/Dashboard.cpp
    src/service/HttpServer.cpp
)
target_include_directories(threadhaul_core PUBLIC
    src/actuator src/telemetry src/config src/logging src/service third_party)
target_link_libraries(threadhaul_core PUBLIC
    advapi32 ws2_32 winhttp wtsapi32 bcrypt user32 shell32)

# --------------------------------------------------------------------------
# Executables
# --------------------------------------------------------------------------
add_executable(ThreadHaulDaemon src/service/main.cpp)
target_link_libraries(ThreadHaulDaemon PRIVATE threadhaul_core)

add_executable(ThreadHaulController src/controller/main.cpp)
target_include_directories(ThreadHaulController PRIVATE third_party)
target_link_libraries(ThreadHaulController PRIVATE ws2_32)

add_executable(critical_simulator tools/simulators/critical_simulator.cpp)
add_executable(batch_simulator tools/simulators/batch_simulator.cpp)

# The service serves the dashboard from <exe dir>\dashboard; a config.json
# next to the binaries lets console mode run straight from the build tree.
add_custom_command(TARGET ThreadHaulDaemon POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/web/dashboard $<TARGET_FILE_DIR:ThreadHaulDaemon>/dashboard
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/config/config.example.json $<TARGET_FILE_DIR:ThreadHaulDaemon>/config.json)

# --------------------------------------------------------------------------
# Tests (ctest)
# --------------------------------------------------------------------------
include(CTest)
if(BUILD_TESTING)
    foreach(test StateMachineTests TelemetryTests ConfigLoggingTests)
        add_executable(${test} tests/${test}.cpp)
        target_link_libraries(${test} PRIVATE threadhaul_core)
        set_target_properties(${test} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
        add_test(NAME ${test} COMMAND ${test})
    endforeach()
endif()

# --------------------------------------------------------------------------
# Package layout (cmake --install, used for the release zip):
#   bin\        ThreadHaulDaemon.exe, ThreadHaulController.exe, dashboard\
#   config\     config.example.json
#   scripts\    install.ps1, uninstall.ps1
#   tools\      simulators, log_viewer.py
# --------------------------------------------------------------------------
install(TARGETS ThreadHaulDaemon ThreadHaulController RUNTIME DESTINATION bin)
install(DIRECTORY web/dashboard/ DESTINATION bin/dashboard)
install(FILES config/config.example.json DESTINATION config)
install(FILES scripts/install.ps1 scripts/uninstall.ps1 DESTINATION scripts)
install(TARGETS critical_simulator batch_simulator RUNTIME DESTINATION tools)
install(FILES tools/log_viewer.py DESTINATION tools)
install(FILES README.md LICENSE CHANGELOG.md DESTINATION .)
install(FILES third_party/nlohmann/LICENSE.MIT DESTINATION licenses RENAME nlohmann-json-LICENSE.MIT)
