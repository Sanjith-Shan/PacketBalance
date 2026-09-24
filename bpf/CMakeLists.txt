# Builds the XDP program to BPF bytecode with clang and generates a libbpf
# skeleton header. Exposes the INTERFACE target `pb_bpf_skel` whose include
# directory holds `packetbalance.skel.h`; the daemon and the data-plane tests
# link it.

find_program(PB_CLANG NAMES clang clang-18 clang-17 clang-16 clang-15 REQUIRED)
# Ubuntu's /usr/sbin/bpftool is a wrapper that dispatches on `uname -r` and fails
# when the running kernel has no linux-tools package (GitHub's Azure kernels, for
# example). Prefer a real versioned binary under /usr/lib/linux-tools when present.
file(GLOB PB_BPFTOOL_VERSIONED /usr/lib/linux-tools/*/bpftool /usr/lib/linux-tools-*/bpftool)
if(PB_BPFTOOL_VERSIONED)
  list(GET PB_BPFTOOL_VERSIONED -1 PB_BPFTOOL_DEFAULT)
  set(PB_BPFTOOL ${PB_BPFTOOL_DEFAULT} CACHE FILEPATH "bpftool binary")
endif()
find_program(PB_BPFTOOL NAMES bpftool REQUIRED)
execute_process(COMMAND ${PB_BPFTOOL} version RESULT_VARIABLE PB_BPFTOOL_RC OUTPUT_QUIET ERROR_QUIET)
if(NOT PB_BPFTOOL_RC EQUAL 0)
  message(FATAL_ERROR "${PB_BPFTOOL} does not run. Install linux-tools-generic and pass -DPB_BPFTOOL=/usr/lib/linux-tools/<ver>/bpftool")
endif()
message(STATUS "bpftool: ${PB_BPFTOOL}")

execute_process(COMMAND uname -m OUTPUT_VARIABLE PB_UNAME_M OUTPUT_STRIP_TRAILING_WHITESPACE)
if(PB_UNAME_M STREQUAL "aarch64" OR PB_UNAME_M STREQUAL "arm64")
  set(PB_BPF_ARCH arm64)
elseif(PB_UNAME_M STREQUAL "x86_64")
  set(PB_BPF_ARCH x86)
else()
  message(FATAL_ERROR "Unsupported BPF target arch: ${PB_UNAME_M}")
endif()

# clang -target bpf does not search the multiarch include directory, where
# Debian/Ubuntu keep <asm/types.h> (pulled in by <linux/types.h>).
# The triplet comes from CMake's C compiler detection (aarch64-linux-gnu,
# x86_64-linux-gnu), falling back to asking the host compiler.
set(PB_BPF_SYS_INCLUDES)
set(PB_MULTIARCH "${CMAKE_LIBRARY_ARCHITECTURE}")
if(NOT PB_MULTIARCH)
  execute_process(COMMAND ${CMAKE_C_COMPILER} -print-multiarch
                  OUTPUT_VARIABLE PB_MULTIARCH OUTPUT_STRIP_TRAILING_WHITESPACE
                  ERROR_QUIET)
endif()
if(PB_MULTIARCH AND EXISTS /usr/include/${PB_MULTIARCH})
  list(APPEND PB_BPF_SYS_INCLUDES -idirafter /usr/include/${PB_MULTIARCH})
endif()

set(PB_BPF_SRC ${CMAKE_CURRENT_SOURCE_DIR}/packetbalance.bpf.c)
set(PB_BPF_OBJ ${CMAKE_CURRENT_BINARY_DIR}/packetbalance.bpf.o)
set(PB_BPF_SKEL ${CMAKE_CURRENT_BINARY_DIR}/packetbalance.skel.h)

add_custom_command(
  OUTPUT ${PB_BPF_OBJ}
  COMMAND ${PB_CLANG} -g -O2 -target bpf -D__TARGET_ARCH_${PB_BPF_ARCH}
          -Wall -Wextra -Werror
          -I${CMAKE_SOURCE_DIR}/include ${PB_BPF_SYS_INCLUDES}
          -c ${PB_BPF_SRC} -o ${PB_BPF_OBJ}
  DEPENDS ${PB_BPF_SRC}
          ${CMAKE_SOURCE_DIR}/include/packetbalance/abi.h
          ${CMAKE_SOURCE_DIR}/include/packetbalance/hash.h
  COMMENT "clang -target bpf packetbalance.bpf.c"
  VERBATIM)

add_custom_command(
  OUTPUT ${PB_BPF_SKEL}
  COMMAND ${PB_BPFTOOL} gen skeleton ${PB_BPF_OBJ} > ${PB_BPF_SKEL}
  DEPENDS ${PB_BPF_OBJ}
  COMMENT "bpftool gen skeleton"
  VERBATIM)

add_custom_target(pb_bpf_obj ALL DEPENDS ${PB_BPF_OBJ} ${PB_BPF_SKEL})

add_library(pb_bpf_skel INTERFACE)
target_include_directories(pb_bpf_skel INTERFACE ${CMAKE_CURRENT_BINARY_DIR})
add_dependencies(pb_bpf_skel pb_bpf_obj)

# Convenience copy so `bpftool prog load build/packetbalance.bpf.o` works by hand.
add_custom_command(TARGET pb_bpf_obj POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy ${PB_BPF_OBJ} ${CMAKE_BINARY_DIR}/packetbalance.bpf.o)
