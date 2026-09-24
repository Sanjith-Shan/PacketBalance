# Control-plane tests against real BPF maps: LbState, RealTable, the ring swap,
# MapReader and the JSON commands, driven through the same map fds the daemon
# uses but with the program never attached and nothing pinned. Linux and root
# (map creation needs CAP_BPF); without root every test is skipped.
add_executable(daemon_test lbstate_test.cpp)
target_link_libraries(daemon_test PRIVATE pb_daemon_lib GTest::gtest_main)

add_test(NAME daemon_test COMMAND daemon_test)
set_tests_properties(daemon_test PROPERTIES
  LABELS needs-root
  SKIP_REGULAR_EXPRESSION "daemon tests need root")
