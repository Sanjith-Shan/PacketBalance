cmake_minimum_required(VERSION 3.22)
project(PacketBalance VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
add_compile_options(-Wall -Wextra)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(PB_ON_LINUX ON)
else()
  set(PB_ON_LINUX OFF)
endif()

# The data plane and the daemon need a Linux kernel, clang with the BPF target,
# bpftool and libbpf. The core library (Maglev, config parsing), pbctl, the
# hash-quality experiment and the Google Test suite for the core build anywhere.
option(PB_BUILD_BPF    "Build the XDP data plane"            ${PB_ON_LINUX})
option(PB_BUILD_DAEMON "Build the packetbalance daemon"      ${PB_ON_LINUX})
option(PB_BUILD_TOOLS  "Build conncheck and lab tools"       ON)
option(PB_BUILD_TESTS  "Build the Google Test suites"        ON)

# Shared ABI header, used by the BPF program and every C++ target.
add_library(pb_abi INTERFACE)
target_include_directories(pb_abi INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_subdirectory(src/core)             # libpb_core: maglev, hash, vip parsing, config

if(PB_BUILD_BPF)
  add_subdirectory(bpf)                # packetbalance.bpf.o + skeleton (target pb_bpf_skel)
endif()

if(PB_BUILD_DAEMON)
  add_subdirectory(src/daemon)         # packetbalance
endif()
add_subdirectory(src/pbctl)            # pbctl (any platform, talks JSON over a unix socket)

if(PB_BUILD_TOOLS)
  add_subdirectory(tools)              # conncheck, conncheck-server, hashquality
endif()

if(PB_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
