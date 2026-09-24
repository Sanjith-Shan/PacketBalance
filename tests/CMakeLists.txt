find_package(GTest REQUIRED)
include(GoogleTest)

add_subdirectory(core)                 # maglev, hash, config: run anywhere
if(PB_BUILD_BPF)
  add_subdirectory(dataplane)          # BPF_PROG_TEST_RUN, Linux + root
endif()
