find_package(GTest REQUIRED)
include(GoogleTest)

add_subdirectory(core)                 # maglev, hash, config: run anywhere
if(PB_BUILD_BPF)
  add_subdirectory(dataplane)          # BPF_PROG_TEST_RUN, Linux + root
endif()
if(PB_BUILD_BPF AND PB_BUILD_DAEMON)
  add_subdirectory(daemon)             # LbState and friends on real maps, Linux + root
endif()
