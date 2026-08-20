cmake_minimum_required(VERSION 3.24)
project(dstDESK VERSION 1.1.3 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(KOBAYASHI_BUILD_TESTS "Build the unit tests" ON)

# ── Sanitizers ───────────────────────────────────────────────────────────────
# An option rather than a build type, because this is not a different product: it is
# the same code with the compiler told to check what it cannot check for free.
#
# It exists because this project had no way to ask the question. Exactly one memory
# fault was ever found here, and it was found by a crash on a user's machine rather
# than by a tool — while an out-of-bounds read in the WAV reader and an unbounded
# allocation beside it sat in a shipped binary, both of which AddressSanitizer names in
# under a second. A build system that owns its own toolchain (decision 11) and cannot
# turn this on has not finished owning it.
#
# Directory scope, so it reaches every target below without each one having to remember.
# -fno-sanitize-recover makes undefined behaviour fail the run rather than print and
# continue: a report nobody notices is the same as no report.
#
# UBSan's vptr check is off. It needs RTTI for every polymorphic type it sees, including
# those inside a Qt that was not built with it, and reports the missing information as a
# fault — so leaving it on trades real findings for noise about someone else's library.
option(KOBAYASHI_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

if(KOBAYASHI_SANITIZE)
    if(MSVC)
        # MSVC ships AddressSanitizer only; there is no UBSan, and /RTC is incompatible
        # with it. Saying so is better than silently giving half of what was asked for.
        message(STATUS "Sanitizers: AddressSanitizer (MSVC has no UndefinedBehaviorSanitizer)")
        add_compile_options(/fsanitize=address)
    else()
        message(STATUS "Sanitizers: AddressSanitizer + UndefinedBehaviorSanitizer")
        add_compile_options(-fsanitize=address,undefined -fno-sanitize=vptr
                            -fno-sanitize-recover=undefined -fno-omit-frame-pointer -g)
        add_link_options(-fsanitize=address,undefined)
    endif()
endif()

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
add_library(kobayashi_core STATIC
    src/Core/Frame.cpp
    src/Core/WavWriter.cpp)

add_dependencies(kobayashi_core dst_protocol_generated)

target_include_directories(kobayashi_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Core)

if(MSVC)
    target_compile_options(kobayashi_core PRIVATE /W4)
else()
    target_compile_options(kobayashi_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ── Application ──────────────────────────────────────────────────────────────
# Qt enters here and nowhere below it: the core above stays framework-free so it
# can be tested without an event loop or a display.
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network WebSockets)

set(CMAKE_AUTOMOC ON)

# Qt 6.8's official macOS binaries pull in the AGL framework, and Apple removed AGL from
# the SDK — so linking any Qt GUI target fails on a current Xcode with
# "ld: framework 'AGL' not found". Nothing here wants it: AGL is a legacy OpenGL surface
# API and this application draws with Widgets.
#
# The reference does not sit on Qt6::Gui. It comes from WrapOpenGL::WrapOpenGL, created by
# Qt's own FindWrapOpenGL.cmake, which Gui links transitively — so the link interface is
# walked rather than a list of likely names being guessed at, since that list was already
# wrong once. Generator expressions are followed too, because Qt wraps most of its private
# dependencies in $<LINK_ONLY:...>.
if(APPLE)
    function(kobayashi_strip_agl target)
        if(NOT TARGET ${target})
            return()
        endif()

        get_property(visited GLOBAL PROPERTY kobayashi_agl_visited)
        if("${target}" IN_LIST visited)
            return()
        endif()
        set_property(GLOBAL APPEND PROPERTY kobayashi_agl_visited "${target}")

        foreach(property INTERFACE_LINK_LIBRARIES IMPORTED_LINK_INTERFACE_LIBRARIES)
            get_target_property(libraries ${target} ${property})
            if(NOT libraries)
                continue()
            endif()

            set(kept "")
            foreach(library IN LISTS libraries)
                if(library MATCHES "(^|[:/ ])AGL(\\.framework)?$")
                    message(STATUS "Dropping AGL from ${target}: ${library}")
                    continue()
                endif()

                list(APPEND kept "${library}")

                # Follow it down: the reference is usually a level or two below.
                set(next "${library}")
                if(next MATCHES "^\\$<LINK_ONLY:(.+)>$")
                    set(next "${CMAKE_MATCH_1}")
                endif()
                if(TARGET ${next})
                    kobayashi_strip_agl(${next})
                endif()
            endforeach()

            set_property(TARGET ${target} PROPERTY ${property} "${kept}")
        endforeach()
    endfunction()

    # Seeded with what this project links; the walk reaches the rest, WrapOpenGL included.
    foreach(qt_target Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::WebSockets)
        kobayashi_strip_agl(${qt_target})
    endforeach()

    # Belt and braces: if Qt's wrapper is present but was never reached through the walk,
    # strip it directly rather than leaving the outcome to the shape of Qt's graph.
    foreach(wrapper WrapOpenGL::WrapOpenGL WrapOpenGL OpenGL::GL OpenGL::OpenGL)
        kobayashi_strip_agl(${wrapper})
    endforeach()
endif()

# Compiled explicitly rather than by CMAKE_AUTORCC.
#
# A .qrc listed as a source of a target does nothing unless AUTORCC is on, and CMake says
# nothing about an unknown source type — so the flag was deleted by an unrelated edit,
# every build after it silently shipped without the icon, and the window and taskbar had
# nothing to show for six releases. This form names the file, generates the source, and
# fails if it is missing. --selftest reports the resources too, so the same slip cannot
# pass unnoticed again.
qt_add_resources(KOBAYASHI_RESOURCES rec/icons/kobayashi.qrc)

add_executable(kobayashi
    ${KOBAYASHI_RESOURCES}
    src/App/main.cpp
    src/App/SelfTest.cpp
    src/App/MainWindow.cpp
    src/App/TranscriptView.cpp
    src/App/TranscriptModel.cpp
    src/App/SettingsDialog.cpp
    src/IO/WsServer.cpp
    src/IO/SttClient.cpp)

if(WIN32)
    # Declared here rather than in project(), which lists CXX only: RC is a Windows
    # language, and naming it unconditionally would have CMake hunt for a resource
    # compiler on platforms that have none. Without it a .rc listed as a source is
    # quietly ignored, and the executable ends up with the default icon while every
    # file involved looks correct.
    enable_language(RC)
    target_sources(kobayashi PRIVATE rec/icons/kobayashi.rc)
endif()

target_include_directories(kobayashi PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/App)

# The version is stated once, by project() above. Repeating it in source is how a
# release ends up disagreeing with the tag that names it.
target_compile_definitions(kobayashi PRIVATE KOBAYASHI_VERSION="${PROJECT_VERSION}")

target_link_libraries(kobayashi PRIVATE
    kobayashi_core
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
    Qt6::WebSockets)

if(MSVC)
    target_compile_options(kobayashi PRIVATE /W4)
else()
    target_compile_options(kobayashi PRIVATE -Wall -Wextra -Wpedantic)
endif()

# A windowed application on each platform's own terms.
#
# Without WIN32_EXECUTABLE the binary is a console application, so every launch — a
# double-click included — brings up an empty black window beside the real one. Without
# MACOSX_BUNDLE it is a plain Unix executable, which macOS cannot launch by
# double-click at all, and the DragNDrop package would carry a bare binary rather than
# something a reviewer can drag into Applications.
#
# kobayashi-sim is deliberately left alone: it is a console tool, and its output is the point.
set_target_properties(kobayashi PROPERTIES
    WIN32_EXECUTABLE TRUE
    MACOSX_BUNDLE    TRUE
    MACOSX_BUNDLE_BUNDLE_NAME      "Kobayashi"
    MACOSX_BUNDLE_BUNDLE_VERSION   "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    MACOSX_BUNDLE_GUI_IDENTIFIER   "com.hrantsp.kobayashi"
    MACOSX_BUNDLE_ICON_FILE        "kobayashi.icns")

# A bundle reads its icon from Contents/Resources by the name in the plist above, so the
# file has to be placed there as part of the target rather than installed alongside it.
if(APPLE)
    set_source_files_properties(rec/icons/kobayashi.icns PROPERTIES
        MACOSX_PACKAGE_LOCATION "Resources")
    target_sources(kobayashi PRIVATE rec/icons/kobayashi.icns)
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
# wherever the project is developed rather than only where it is needed. It is the
# fallback for the deployment below: if windeployqt is missing, this is what still
# lets `dst.py run` start the application.
file(GENERATE
     OUTPUT "$<TARGET_FILE_DIR:kobayashi>/qt-runtime-dir.txt"
     CONTENT "$<TARGET_FILE_DIR:Qt6::Core>")

# Windows: make the built executable start on its own, not only through dst.py.
#
# Putting Qt on PATH covers `dst.py run`, but double-clicking the binary is the first
# thing anyone tries, and the loader then finds no Qt6Widgets.dll. Copying the DLLs
# beside it is not enough either: Qt locates its plugins relative to Qt6Core.dll, so a
# copy next to the executable sends it looking for plugins in a directory that does not
# exist, and the application fails to create a window instead — a worse error, because
# it is further from the cause. windeployqt gets both halves right, and it is the same
# tool the install-time deployment further down dispatches to.
if(WIN32)
    find_program(KOBAYASHI_WINDEPLOYQT windeployqt
                 HINTS "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_BINS}")
    if(KOBAYASHI_WINDEPLOYQT)
        add_custom_command(TARGET kobayashi POST_BUILD
            COMMAND "${KOBAYASHI_WINDEPLOYQT}" --no-translations --no-compiler-runtime
                    "$<TARGET_FILE:kobayashi>"
            COMMENT "Copying the Qt runtime beside kobayashi.exe"
            VERBATIM)
    else()
        message(WARNING
            "windeployqt was not found beside Qt, so the built executable will start "
            "only through `dst.py run`, which puts Qt on PATH. A packaged build is "
            "unaffected.")
    endif()
endif()

# ── Simulator ────────────────────────────────────────────────────────────────
# A separate executable, not a mode of kobayashi. It is a client of the protocol —
# its real counterpart is a browser — and keeping it apart stops the server's
# command line carrying options that only apply when it is not a server.
add_executable(kobayashi-sim
    src/Sim/main.cpp
    src/Sim/Simulate.cpp)

target_include_directories(kobayashi-sim PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/Sim)
target_compile_definitions(kobayashi-sim PRIVATE KOBAYASHI_VERSION="${PROJECT_VERSION}")

target_link_libraries(kobayashi-sim PRIVATE
    kobayashi_core
    Qt6::Core
    Qt6::Network
    Qt6::WebSockets)

if(MSVC)
    target_compile_options(kobayashi-sim PRIVATE /W4)
else()
    target_compile_options(kobayashi-sim PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ── Tests ────────────────────────────────────────────────────────────────────
if(KOBAYASHI_BUILD_TESTS)
    find_package(Catch2 3 REQUIRED)
    enable_testing()

    add_executable(kobayashi_tests
        tst/TestFrame.cpp
        tst/TestRecorder.cpp
        tst/TestTranscript.cpp)
    target_link_libraries(kobayashi_tests PRIVATE kobayashi_core Catch2::Catch2WithMain)

    include(Catch)
    catch_discover_tests(kobayashi_tests)

    # The transcript benchmark needs Qt and a view, so it cannot live in the suite
    # above — that one links kobayashi_core alone precisely so it runs with no display
    # and no Qt libraries present. This is a separate binary for that reason.
    #
    # It compiles the view's sources directly rather than linking the application:
    # `kobayashi` is an executable, and turning it into a library to share two files
    # would reshape the build for the benefit of a benchmark.
    add_executable(kobayashi-bench
        tst/BenchTranscript.cpp
        src/App/TranscriptView.cpp
        src/App/TranscriptModel.cpp)
    target_include_directories(kobayashi-bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(kobayashi-bench PRIVATE kobayashi_core Qt6::Widgets)
    if(MSVC)
        target_compile_options(kobayashi-bench PRIVATE /W4)
    else()
        target_compile_options(kobayashi-bench PRIVATE -Wall -Wextra)
    endif()

    # The transcription service, made to misbehave. SttClient's reconnection only runs
    # when a connection dies unexpectedly, and the live service will not do that on
    # request — so without this the reconnect path could not be exercised at all, which
    # is how three separate faults in it survived being written.
    add_executable(kobayashi-mockstt tst/MockStt.cpp)
    target_include_directories(kobayashi-mockstt PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(kobayashi-mockstt PRIVATE kobayashi_core Qt6::Core Qt6::WebSockets)
    add_dependencies(kobayashi-mockstt dst_protocol_generated)
    if(MSVC)
        target_compile_options(kobayashi-mockstt PRIVATE /W4)
    else()
        target_compile_options(kobayashi-mockstt PRIVATE -Wall -Wextra)
    endif()

    # Registered as a test because the property it guards is a correctness property, not
    # a preference: a transcript view whose cost grows with the transcript stops keeping
    # up during a long meeting. The assertion is on shape rather than milliseconds, so it
    # does not depend on how fast the machine running it happens to be — see the comment
    # at the top of BenchTranscript.cpp.
    add_test(NAME transcript-scaling COMMAND kobayashi-bench --check)
    set_tests_properties(transcript-scaling PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        TIMEOUT 300)
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
    set(KOBAYASHI_INSTALL_RPATH "@executable_path/../lib")
elseif(UNIX)
    set(KOBAYASHI_INSTALL_RPATH "$ORIGIN/../lib")
endif()

if(DEFINED KOBAYASHI_INSTALL_RPATH)
    set_target_properties(kobayashi kobayashi-sim PROPERTIES
        INSTALL_RPATH "${KOBAYASHI_INSTALL_RPATH}"
        BUILD_WITH_INSTALL_RPATH FALSE)
endif()

# Desktop integration, which on Linux is the only thing that produces an icon at all:
# a file manager will not read one out of an ELF binary the way Explorer reads a
# resource section, so the icon comes from an installed .desktop entry and the icon
# theme, or it does not exist.
if(UNIX AND NOT APPLE)
    install(FILES rec/icons/kobayashi.desktop
            DESTINATION share/applications)
    foreach(KOBAYASHI_ICON_SIZE IN ITEMS 16 32 48 64 128 256)
        install(FILES rec/icons/kobayashi-${KOBAYASHI_ICON_SIZE}.png
                DESTINATION share/icons/hicolor/${KOBAYASHI_ICON_SIZE}x${KOBAYASHI_ICON_SIZE}/apps
                RENAME kobayashi.png)
    endforeach()
endif()

install(TARGETS kobayashi kobayashi-sim
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        BUNDLE  DESTINATION .)

# Qt's own deployment tooling collects the libraries and plugins the binary loads,
# including the platform and TLS plugins that are opened by name at runtime and so
# appear in no link line. It dispatches to windeployqt or macdeployqt as appropriate;
# Linux support is partial, hence NO_UNSUPPORTED_PLATFORM_ERROR rather than a hard
# failure on the platform this is developed on.
qt_generate_deploy_app_script(
    TARGET kobayashi
    OUTPUT_SCRIPT KOBAYASHI_DEPLOY_SCRIPT
    NO_UNSUPPORTED_PLATFORM_ERROR)
install(SCRIPT ${KOBAYASHI_DEPLOY_SCRIPT})

# Plugins are loaded by name at runtime, so they appear in no link line and the
# deployment script does not collect them on every platform. Two groups matter:
# without `platforms` the application cannot open a window at all, and without `tls`
# every secure connection fails — a failure that surfaces on the first transcription
# request rather than at startup.
foreach(KOBAYASHI_PLUGIN_GROUP IN ITEMS platforms tls)
    if(EXISTS "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_PLUGINS}/${KOBAYASHI_PLUGIN_GROUP}")
        install(DIRECTORY "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_PLUGINS}/${KOBAYASHI_PLUGIN_GROUP}"
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
        file(GLOB KOBAYASHI_OPENSSL_RUNTIME
             "${openssl_LIB_DIRS_RELEASE}/libssl${CMAKE_SHARED_LIBRARY_SUFFIX}*"
             "${openssl_LIB_DIRS_RELEASE}/libcrypto${CMAKE_SHARED_LIBRARY_SUFFIX}*")
        if(KOBAYASHI_OPENSSL_RUNTIME)
            install(FILES ${KOBAYASHI_OPENSSL_RUNTIME} DESTINATION lib)
        else()
            message(STATUS
                "OpenSSL resolved to a static build, so nothing is shipped for it. The "
                "packaged application will use whatever OpenSSL the target machine has, "
                "and kobayashi --selftest reports whether that works.")
        endif()
    endif()
endif()

set(CPACK_PACKAGE_NAME "Kobayashi")
set(CPACK_PACKAGE_VENDOR "hrantsp")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Dual-stream meeting transcription")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "Kobayashi")
set(CPACK_PACKAGE_FILE_NAME
    "Kobayashi-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_VERBATIM_VARIABLES ON)

include(CPack)
