cmake_minimum_required(VERSION 3.24)
project(dstDESK LANGUAGES CXX)

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
find_package(Qt6 REQUIRED COMPONENTS Core Network WebSockets)

set(CMAKE_AUTOMOC ON)

add_executable(dstdesk
    src/App/main.cpp
    src/App/SelfTest.cpp)

target_include_directories(dstdesk PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/App)

target_link_libraries(dstdesk PRIVATE
    dstdesk_core
    Qt6::Core
    Qt6::Network
    Qt6::WebSockets)

if(MSVC)
    target_compile_options(dstdesk PRIVATE /W4)
else()
    target_compile_options(dstdesk PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ── Tests ────────────────────────────────────────────────────────────────────
if(DSTDESK_BUILD_TESTS)
    find_package(Catch2 3 REQUIRED)
    enable_testing()

    add_executable(dstdesk_tests
        tst/TestFrame.cpp
        tst/TestRecorder.cpp)
    target_link_libraries(dstdesk_tests PRIVATE dstdesk_core Catch2::Catch2WithMain)

    include(Catch)
    catch_discover_tests(dstdesk_tests)
endif()
