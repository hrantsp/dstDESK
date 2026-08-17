cmake_minimum_required(VERSION 3.24)
project(dstDESK VERSION 1.0.5 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(DSTDESK_BUILD_TESTS "Build the unit tests" ON)

# ── Generated protocol constants ─────────────────────────────────────────────
# dstDESK/rec/protocol.json is the single source of truth for both halves of
# the pipeline. The header below and dstORCH's protocol.js are generated from it;
# neither is edited by hand. See decision 3 in dstOMNI/DESIGN.md.
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(DST_PROTOCOL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/rec)
set(DST_GENERATED_HEADER ${CMAKE_CURRENT_SOURCE_DIR}/src/Core/Protocol.hpp)
set(DST_ORCH_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../dstORCH)

set(DST_GENERATE_ARGS --cpp-out ${DST_GENERATED_HEADER})
set(DST_GENERATED_OUTPUTS ${DST_GENERATED_HEADER})

# dstORCH is generated into only when it is actually checked out beside dstDESK.
# Without this guard, building dstDESK alone would create a stray ../dstORCH tree.
if(IS_DIRECTORY ${DST_ORCH_DIR})
    set(DST_GENERATED_JS ${DST_ORCH_DIR}/src/generated/protocol.js)
    list(APPEND DST_GENERATE_ARGS --js-out ${DST_GENERATED_JS})
    list(APPEND DST_GENERATED_OUTPUTS ${DST_GENERATED_JS})
else()
    message(STATUS
        "dstORCH not found beside dstDESK — generating C++ constants only. "
        "The extension will need protocol.js generated before it can run; see "
        "dstOMNI/README.md for the workspace layout.")
endif()

# The command's declared output is a stamp file, not the generated sources.
#
# generate.py leaves a file untouched when its content would be identical, so that
# editing an unrelated part of protocol.json does not recompile everything that
# includes the header. That means the header can legitimately be older than
# protocol.json, and naming it as the OUTPUT would make CMake believe it is
# perpetually stale and re-run the generator on every single build.
#
# The stamp is always touched, so CMake's dependency check is satisfied; the header
# still changes only when its content really changes, so compilation is still skipped
# when nothing meaningful moved.
set(DST_PROTOCOL_STAMP ${CMAKE_CURRENT_BINARY_DIR}/protocol_generated.stamp)

add_custom_command(
    OUTPUT ${DST_PROTOCOL_STAMP}
    COMMAND ${Python3_EXECUTABLE} ${DST_PROTOCOL_DIR}/generate.py ${DST_GENERATE_ARGS}
    COMMAND ${CMAKE_COMMAND} -E touch ${DST_PROTOCOL_STAMP}
    DEPENDS ${DST_PROTOCOL_DIR}/protocol.json ${DST_PROTOCOL_DIR}/generate.py
    BYPRODUCTS ${DST_GENERATED_OUTPUTS}
    COMMENT "Generating protocol constants from protocol.json"
    VERBATIM)

add_custom_target(dst_protocol_generated ALL DEPENDS ${DST_PROTOCOL_STAMP})

# ── Core: no Qt, so it can be tested without an event loop or a display ──────
add_library(dstdesk_core STATIC
    src/Core/Frame.cpp
    src/Core/WavWriter.cpp)

add_dependencies(dstdesk_core dst_protocol_generated)

target_include_directories(dstdesk_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Core)

if(MSVC)
    target_compile_options(dstdesk_core PRIVATE /W4)
else()
    target_compile_options(dstdesk_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ── Application ──────────────────────────────────────────────────────────────
# Qt enters here and nowhere below it: the core above stays framework-free so it
# can be tested without an event loop or a display.
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network WebSockets)

set(CMAKE_AUTOMOC ON)

add_executable(dstdesk
    src/App/main.cpp
    src/App/SelfTest.cpp
    src/App/MainWindow.cpp
    src/App/TranscriptView.cpp
    src/App/SettingsDialog.cpp
    src/IO/WsServer.cpp
    src/IO/SttClient.cpp)

target_include_directories(dstdesk PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/App)

# The version is stated once, by project() above. Repeating it in source is how a
# release ends up disagreeing with the tag that names it.
target_compile_definitions(dstdesk PRIVATE DSTDESK_VERSION="${PROJECT_VERSION}")

target_link_libraries(dstdesk PRIVATE
    dstdesk_core
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
    Qt6::WebSockets)

if(MSVC)
    target_compile_options(dstdesk PRIVATE /W4)
else()
    target_compile_options(dstdesk PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Where Qt's runtime lives, recorded beside the executable at configure time.
#
# ELF and Mach-O binaries carry a search path, so a build-tree binary finds the Qt it
# was linked against and simply runs. Windows has no equivalent: the loader looks in the
# executable's own directory and then along PATH, so the same binary fails with
# "Qt6Widgets.dll was not found" — a message that names a symptom and no cause. Writing
# the directory here lets `dst.py run` prepend it, which beats asking Conan at run time
# where the package landed: this is the path the linker actually used.
#
# Written on every platform, not only Windows, so the mechanism can be exercised
# wherever the project is developed rather than only where it is needed.
file(GENERATE
     OUTPUT "$<TARGET_FILE_DIR:dstdesk>/qt-runtime-dir.txt"
     CONTENT "$<TARGET_FILE_DIR:Qt6::Core>")

# ── Simulator ────────────────────────────────────────────────────────────────
# A separate executable, not a mode of dstdesk. It is a client of the protocol —
# its real counterpart is a browser — and keeping it apart stops the server's
# command line carrying options that only apply when it is not a server.
add_executable(dstsim
    src/Sim/main.cpp
    src/Sim/Simulate.cpp)

target_include_directories(dstsim PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/Sim)
target_compile_definitions(dstsim PRIVATE DSTDESK_VERSION="${PROJECT_VERSION}")

target_link_libraries(dstsim PRIVATE
    dstdesk_core
    Qt6::Core
    Qt6::Network
    Qt6::WebSockets)

if(MSVC)
    target_compile_options(dstsim PRIVATE /W4)
else()
    target_compile_options(dstsim PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ── Tests ────────────────────────────────────────────────────────────────────
if(DSTDESK_BUILD_TESTS)
    find_package(Catch2 3 REQUIRED)
    enable_testing()

    add_executable(dstdesk_tests
        tst/TestFrame.cpp
        tst/TestRecorder.cpp
        tst/TestTranscript.cpp)
    target_link_libraries(dstdesk_tests PRIVATE dstdesk_core Catch2::Catch2WithMain)

    include(Catch)
    catch_discover_tests(dstdesk_tests)
endif()

# ── Packaging ────────────────────────────────────────────────────────────────
# One artifact that runs on a machine with no toolchain, which is what "package"
# has to mean — a bare executable would only run where Qt already happens to be.
include(GNUInstallDirs)

# The deployed layout puts binaries in bin/ and the Qt libraries in lib/, so the
# executables need a relative run path to find them. Without this the archive
# contains every library it needs and still refuses to start anywhere but the machine
# that built it — which is the failure "packaged" is supposed to rule out.
if(APPLE)
    set(DSTDESK_INSTALL_RPATH "@executable_path/../lib")
elseif(UNIX)
    set(DSTDESK_INSTALL_RPATH "$ORIGIN/../lib")
endif()

if(DEFINED DSTDESK_INSTALL_RPATH)
    set_target_properties(dstdesk dstsim PROPERTIES
        INSTALL_RPATH "${DSTDESK_INSTALL_RPATH}"
        BUILD_WITH_INSTALL_RPATH FALSE)
endif()

install(TARGETS dstdesk dstsim
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        BUNDLE  DESTINATION .)

# Qt's own deployment tooling collects the libraries and plugins the binary loads,
# including the platform and TLS plugins that are opened by name at runtime and so
# appear in no link line. It dispatches to windeployqt or macdeployqt as appropriate;
# Linux support is partial, hence NO_UNSUPPORTED_PLATFORM_ERROR rather than a hard
# failure on the platform this is developed on.
qt_generate_deploy_app_script(
    TARGET dstdesk
    OUTPUT_SCRIPT DSTDESK_DEPLOY_SCRIPT
    NO_UNSUPPORTED_PLATFORM_ERROR)
install(SCRIPT ${DSTDESK_DEPLOY_SCRIPT})

# Plugins are loaded by name at runtime, so they appear in no link line and the
# deployment script does not collect them on every platform. Two groups matter:
# without `platforms` the application cannot open a window at all, and without `tls`
# every secure connection fails — a failure that surfaces on the first transcription
# request rather than at startup.
foreach(DSTDESK_PLUGIN_GROUP IN ITEMS platforms tls)
    if(EXISTS "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_PLUGINS}/${DSTDESK_PLUGIN_GROUP}")
        install(DIRECTORY "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_PLUGINS}/${DSTDESK_PLUGIN_GROUP}"
                DESTINATION plugins
                FILES_MATCHING PATTERN "*${CMAKE_SHARED_LIBRARY_SUFFIX}*")
    endif()
endforeach()

# Tells Qt where those plugins are, relative to the executable. Without it Qt looks
# where it was built, which does not exist on the machine the package is opened on.
# Written into a staging subdirectory, never beside the built binaries. Qt reads
# qt.conf next to the executable, so leaving one in the build tree would make the
# development build follow the packaged layout and look for plugins that are only
# there after installation.
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/deploy/qt.conf" "[Paths]\nPlugins = ../plugins\n")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/deploy/qt.conf" DESTINATION ${CMAKE_INSTALL_BINDIR})

# Qt's TLS plugin opens OpenSSL by name; Qt bundles none. Shipping the one Conan
# resolved makes secure connections a property of the package rather than of whatever
# the receiving machine happens to have. See decision 12 in dstOMNI/DESIGN.md.
#
# Not on Windows, where Qt ships a Schannel backend and uses the platform's own TLS
# stack. There is nothing to ship, and the recipe does not require OpenSSL at all, so
# asking for it here would fail the configure step.
if(NOT WIN32)
    find_package(OpenSSL REQUIRED)
    # Conan's OpenSSL targets are interface aggregates rather than imported libraries,
    # so IMPORTED_RUNTIME_ARTIFACTS does not apply; the files are taken from the package
    # directory CMakeDeps reports. Only shared builds need shipping — a static OpenSSL
    # is already inside whatever linked it, and nothing here links it at all.
    if(DEFINED openssl_LIB_DIRS_RELEASE)
        file(GLOB DSTDESK_OPENSSL_RUNTIME
             "${openssl_LIB_DIRS_RELEASE}/libssl${CMAKE_SHARED_LIBRARY_SUFFIX}*"
             "${openssl_LIB_DIRS_RELEASE}/libcrypto${CMAKE_SHARED_LIBRARY_SUFFIX}*")
        if(DSTDESK_OPENSSL_RUNTIME)
            install(FILES ${DSTDESK_OPENSSL_RUNTIME} DESTINATION lib)
        else()
            message(STATUS
                "OpenSSL resolved to a static build, so nothing is shipped for it. The "
                "packaged application will use whatever OpenSSL the target machine has, "
                "and dstdesk --selftest reports whether that works.")
        endif()
    endif()
endif()

set(CPACK_PACKAGE_NAME "dstDESK")
set(CPACK_PACKAGE_VENDOR "hrantsp")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Dual-stream meeting transcription")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "dstDESK")
set(CPACK_PACKAGE_FILE_NAME
    "dstDESK-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_VERBATIM_VARIABLES ON)

include(CPack)
