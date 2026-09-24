# Core unit tests: Maglev, the shared flow hash, VIP parsing. Portable, no
# kernel needed, so they run on macOS, in the lab VM and in CI.
add_executable(pb_core_tests
  maglev_test.cpp
  hash_test.cpp
  vipspec_test.cpp
  config_test.cpp)
target_link_libraries(pb_core_tests PRIVATE pb_core GTest::gtest_main)
gtest_discover_tests(pb_core_tests)
