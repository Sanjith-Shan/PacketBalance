# conncheck and conncheck-server: the long-lived TCP connection survival
# instrument for Experiments 3 (backend churn) and 4 (LB failover).
# Linux only: both use epoll. On other platforms the targets are skipped.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(STATUS "conncheck: epoll is Linux only, skipping")
  return()
endif()

add_executable(conncheck conncheck.cpp)
target_compile_features(conncheck PRIVATE cxx_std_20)

add_executable(conncheck-server conncheck_server.cpp)
target_compile_features(conncheck-server PRIVATE cxx_std_20)
