# packetbalance: the control-plane daemon. Linux only (libbpf, XDP).
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBBPF REQUIRED IMPORTED_TARGET libbpf)
find_package(yaml-cpp REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(Threads REQUIRED)

# Everything but main(), as a library so tests/daemon can drive LbState and
# MapReader against real (unpinned) maps.
add_library(pb_daemon_lib STATIC
  options.cpp
  daemon.cpp
  log.cpp
  loader.cpp
  map_reader.cpp
  ring_map.cpp
  real_table.cpp
  lb_state.cpp
  neighbor_resolver.cpp
  health_checker.cpp
  commands.cpp
  api_server.cpp
  metrics.cpp
  metrics_server.cpp)
target_include_directories(pb_daemon_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pb_daemon_lib PUBLIC
  pb_core
  pb_bpf_skel
  PkgConfig::LIBBPF
  nlohmann_json::nlohmann_json
  Threads::Threads)
if(TARGET yaml-cpp::yaml-cpp)
  target_link_libraries(pb_daemon_lib PUBLIC yaml-cpp::yaml-cpp)
else()
  target_link_libraries(pb_daemon_lib PUBLIC yaml-cpp)
endif()

add_executable(packetbalance main.cpp)
target_link_libraries(packetbalance PRIVATE pb_daemon_lib)

install(TARGETS packetbalance RUNTIME DESTINATION sbin)
