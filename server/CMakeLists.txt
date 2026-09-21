# W2F server build for every platform (Windows / Linux / macOS; the same sources are meant to be wrapped by Unreal Engine 5 later).
# The Makefile next to this file is the quick way to build and test on a Mac; CMake is what CI uses and what a Windows machine needs.
#
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug            # Debug + AddressSanitizer + UBSan (the default for Debug)
#   cmake --build build
#   ctest --test-dir build --output-on-failure
#
#   cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DW2F_SANITIZE=OFF
#
# Layout (unchanged): src/ + include/w2f/  = the engine (rules, combat simulation, shop, economy). It knows NOTHING about networking or
# Unreal; the isolation test in ctest fails if that ever changes.  net/ = sockets, WebSocket, lobby, protocol.  tools/ = server + demo.

cmake_minimum_required(VERSION 3.16)
project(w2f VERSION 0.11.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Debug" OR CMAKE_CONFIGURATION_TYPES)
  set(_w2f_sanitize_default ON)
else()
  set(_w2f_sanitize_default OFF)
endif()
option(W2F_SANITIZE "Build with AddressSanitizer (+ UndefinedBehaviorSanitizer where the compiler has it)" ${_w2f_sanitize_default})
option(W2F_WERROR "Treat compiler warnings as errors" OFF)
option(W2F_UE_COMPAT "Build the way Unreal Engine builds game code: no C++ exceptions, no RTTI" OFF)

# ---- flags shared by every target ----------------------------------------------------------------------------------------------------
add_library(w2f_options INTERFACE)
if(MSVC)
  # /permissive-: standard conformance. NOMINMAX / LEAN_AND_MEAN keep <windows.h> from breaking std::min / std::max.
  target_compile_options(w2f_options INTERFACE /W4 /permissive- /utf-8 /EHsc)
  target_compile_definitions(w2f_options INTERFACE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
  if(W2F_WERROR)
    target_compile_options(w2f_options INTERFACE /WX)
  endif()
  if(W2F_SANITIZE)
    # MSVC's ASan is incompatible with the runtime checks and edit-and-continue that CMake's Debug flags enable.
    foreach(_var CMAKE_CXX_FLAGS_DEBUG CMAKE_CXX_FLAGS_RELWITHDEBINFO)
      string(REPLACE "/RTC1" "" ${_var} "${${_var}}")
      string(REPLACE "/ZI" "/Zi" ${_var} "${${_var}}")
    endforeach()
    target_compile_options(w2f_options INTERFACE /fsanitize=address /Zi)
    target_link_options(w2f_options INTERFACE /INCREMENTAL:NO)
  endif()
else()
  target_compile_options(w2f_options INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wconversion)
  if(W2F_WERROR)
    target_compile_options(w2f_options INTERFACE -Werror)
  endif()
  if(W2F_SANITIZE)
    # An undefined-behaviour report fails the run (no "recover"); ASan aborts on its own. -O1 in Debug: the sanitized tests are ~5x faster
    # than at -O0 (assertions stay on).
    target_compile_options(w2f_options INTERFACE -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer
                           $<$<CONFIG:Debug>:-O1>)
    target_link_options(w2f_options INTERFACE -fsanitize=address,undefined)
  endif()
endif()

if(W2F_UE_COMPAT)
  # The engine is meant to be dropped into an Unreal module: UE compiles without exceptions and without RTTI, so the whole code base
  # (tests included) must too. CI builds and runs everything this way in one job.
  if(MSVC)
    target_compile_options(w2f_options INTERFACE /GR-)   # (MSVC: RTTI off only; the exception-free STL configuration is not exercised here)
  else()
    target_compile_options(w2f_options INTERFACE -fno-exceptions -fno-rtti)
  endif()
endif()

set(W2F_DATA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/data")
set(W2F_TEST_DATA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
set(W2F_SCHEMA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/docs/schemas")
set(W2F_GOLDEN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tests/golden")

# ---- the engine: headless, integer-only, deterministic; no networking ---------------------------------------------------------------------
file(GLOB W2F_ENGINE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
add_library(w2f_engine STATIC ${W2F_ENGINE_SOURCES})
target_include_directories(w2f_engine PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(w2f_engine PUBLIC w2f_options)

# ---- the network layer: WebSocket server, lobby, JSON protocol (uses only the engine's public headers) --------------------------------------
file(GLOB W2F_NET_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/net/src/*.cpp")
add_library(w2f_net STATIC ${W2F_NET_SOURCES})
target_include_directories(w2f_net PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/net/include")
target_link_libraries(w2f_net PUBLIC w2f_engine)
if(WIN32)
  target_link_libraries(w2f_net PUBLIC ws2_32)
endif()

find_package(Threads REQUIRED)

# ---- tests, demo, server -----------------------------------------------------------------------------------------------------------------
function(w2f_data_defs target)
  target_compile_definitions(${target} PRIVATE
    W2F_DATA_DIR="${W2F_DATA_DIR}"
    W2F_TEST_DATA_DIR="${W2F_TEST_DATA_DIR}"
    W2F_SCHEMA_DIR="${W2F_SCHEMA_DIR}"
    W2F_GOLDEN_DIR="${W2F_GOLDEN_DIR}")
endfunction()

# The engine's own tests link ONLY the engine: if the engine ever needed the network layer, this would stop linking.
add_executable(w2f_tests tests/tests.cpp)
target_include_directories(w2f_tests PRIVATE tests/support)
target_link_libraries(w2f_tests PRIVATE w2f_engine)
w2f_data_defs(w2f_tests)

add_executable(w2f_net_tests net/tests/net_tests.cpp)
target_include_directories(w2f_net_tests PRIVATE tests/support)
target_link_libraries(w2f_net_tests PRIVATE w2f_net Threads::Threads)
w2f_data_defs(w2f_net_tests)

add_executable(w2f_demo tools/sim_demo.cpp)
target_include_directories(w2f_demo PRIVATE tests/support)
target_link_libraries(w2f_demo PRIVATE w2f_engine)
w2f_data_defs(w2f_demo)

add_executable(w2f_server tools/w2f_server.cpp)
target_link_libraries(w2f_server PRIVATE w2f_net)
w2f_data_defs(w2f_server)

# The bulk match simulator behind the balance passes (docs/balance.md).
add_executable(w2f_balance tools/balance_sim.cpp)
target_link_libraries(w2f_balance PRIVATE w2f_engine)
w2f_data_defs(w2f_balance)

enable_testing()
add_test(NAME isolation
         COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_isolation.cmake)
add_test(NAME engine COMMAND w2f_tests)
add_test(NAME net COMMAND w2f_net_tests)
add_test(NAME balance_smoke COMMAND w2f_balance --matches 2)   # the tool runs and reads the production data
