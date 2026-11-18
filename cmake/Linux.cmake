# Linux build (included from CMakeLists.txt on non-Windows hosts).
# The engine and service share their logic with Windows; the platform layer
# lives in src/platform/linux. Needs g++ (C++17) and OpenSSL.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

add_compile_options(-Wall -Wextra)
# GCC 13+ flags "const json& v = Field(obj, key, path)" because a temporary
# string is passed, though the reference points into obj; a false positive.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 13)
    add_compile_options(-Wno-dangling-reference)
endif()

add_library(threadhaul_core STATIC
    src/actuator/ActuatorManager.cpp
    src/actuator/StateMachine.cpp
    src/telemetry/AdaptiveThresholds.cpp
    src/telemetry/CpuMath.cpp
    src/telemetry/ProcessIdentity.cpp
    src/telemetry/ProcessMatch.cpp
    src/telemetry/Signals.cpp
    src/config/Config.cpp
    src/config/Secrets.cpp
    src/logging/EventLogWriter.cpp
    src/logging/JsonLinesLogger.cpp
    src/logging/LogChain.cpp
    src/service/AutoProtect.cpp
    src/service/ConfigSecurity.cpp
    src/service/Coordination.cpp
    src/service/Daemon.cpp
    src/service/Dashboard.cpp
    src/service/HttpServer.cpp
    src/service/Metrics.cpp
    src/service/SelfCheck.cpp
    src/common/SecureChannel.cpp
    src/platform/linux/LinuxActuator.cpp
    src/platform/linux/LinuxAlerts.cpp
    src/platform/linux/LinuxControlSocket.cpp
    src/platform/linux/LinuxSignals.cpp
    src/platform/linux/LinuxTelemetry.cpp
)
target_include_directories(threadhaul_core PUBLIC
    src/actuator src/telemetry src/config src/logging src/service src/common src/platform/linux third_party)
target_link_libraries(threadhaul_core PUBLIC OpenSSL::SSL OpenSSL::Crypto Threads::Threads)

add_executable(threadhauld src/service/main.cpp)
target_link_libraries(threadhauld PRIVATE threadhaul_core)

# Dashboard pages beside the binary, so a build tree runs as is.
add_custom_command(TARGET threadhauld POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/web/dashboard $<TARGET_FILE_DIR:threadhauld>/dashboard)

include(CTest)
if(BUILD_TESTING)
    foreach(test LinuxEngineTests LinuxServiceTests)
        add_executable(${test} tests/${test}.cpp)
        target_link_libraries(${test} PRIVATE threadhaul_core)
        set_target_properties(${test} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
        # From the source tree: the service tests read config/ examples.
        add_test(NAME ${test} COMMAND ${test} WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    endforeach()
endif()

include(GNUInstallDirs)
install(TARGETS threadhauld RUNTIME DESTINATION ${CMAKE_INSTALL_SBINDIR})
install(DIRECTORY web/dashboard/ DESTINATION ${CMAKE_INSTALL_DATADIR}/threadhaul/dashboard)
install(FILES packaging/linux/threadhaul.service DESTINATION lib/systemd/system)
install(FILES config/config.linux.example.json DESTINATION ${CMAKE_INSTALL_DATADIR}/threadhaul
        RENAME config.example.json)
install(FILES LICENSE README.md DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/threadhaul)
install(FILES third_party/nlohmann/LICENSE.MIT DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/threadhaul
        RENAME nlohmann-json-LICENSE.MIT)

# Packages: cpack -G DEB / cpack -G RPM (from the build folder). Both
# install to /usr, ship the systemd unit, and create /etc/threadhaul/config.json
# from the example on first install; the service is enabled by the admin.
set(CPACK_PACKAGE_NAME threadhaul)
set(CPACK_PACKAGE_VENDOR "ThreadHaul")
set(CPACK_PACKAGE_CONTACT "https://github.com/Hikunokyojin/ThreadHaul-Daemon/issues")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Keeps latency-critical workloads responsive under CPU pressure")
set(CPACK_PACKAGE_DESCRIPTION
    "ThreadHaul watches critical processes and, when they are short of CPU, lowers the priority of or pauses batch processes, restoring them when the pressure passes. Runs as a systemd service with a local web dashboard.")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/Hikunokyojin/ThreadHaul-Daemon")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGING_INSTALL_PREFIX /usr)
set(CPACK_RESOURCE_FILE_LICENSE ${CMAKE_SOURCE_DIR}/LICENSE)
set(CPACK_STRIP_FILES ON)

set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_SECTION admin)
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_DEPENDS "systemd")
set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
    ${CMAKE_SOURCE_DIR}/packaging/linux/postinst
    ${CMAKE_SOURCE_DIR}/packaging/linux/prerm
    ${CMAKE_SOURCE_DIR}/packaging/linux/postrm)
set(CPACK_DEBIAN_PACKAGE_CONTROL_STRICT_PERMISSION ON)

set(CPACK_RPM_FILE_NAME RPM-DEFAULT)
set(CPACK_RPM_PACKAGE_LICENSE "Apache-2.0")
set(CPACK_RPM_PACKAGE_GROUP "System Environment/Daemons")
set(CPACK_RPM_PACKAGE_AUTOREQ ON)
set(CPACK_RPM_PACKAGE_REQUIRES "systemd")
set(CPACK_RPM_POST_INSTALL_SCRIPT_FILE ${CMAKE_SOURCE_DIR}/packaging/linux/postinst)
set(CPACK_RPM_PRE_UNINSTALL_SCRIPT_FILE ${CMAKE_SOURCE_DIR}/packaging/linux/prerm)
set(CPACK_RPM_POST_UNINSTALL_SCRIPT_FILE ${CMAKE_SOURCE_DIR}/packaging/linux/postrm)
# Directories every system already owns.
set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION /usr/lib/systemd /usr/lib/systemd/system /usr/share/doc)

include(CPack)
