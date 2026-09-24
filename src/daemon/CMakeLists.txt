# packetbalance: the control-plane daemon. Linux only (libbpf, XDP).
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBBPF REQUIRED IMPORTED_TARGET libbpf)
find_package(yaml-cpp REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(Threads REQUIRED)

add_executable(packetbalance
  main.cpp
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

target_link_libraries(packetbalance PRIVATE
  pb_core
  pb_bpf_skel
  PkgConfig::LIBBPF
  nlohmann_json::nlohmann_json
  Threads::Threads)
if(TARGET yaml-cpp::yaml-cpp)
  target_link_libraries(packetbalance PRIVATE yaml-cpp::yaml-cpp)
else()
  target_link_libraries(packetbalance PRIVATE yaml-cpp)
endif()

install(TARGETS packetbalance RUNTIME DESTINATION sbin)
