# BPF_PROG_TEST_RUN tests for the XDP program. Linux only, needs root. Without
# root every test calls GTEST_SKIP with "dataplane tests need root" and ctest
# reports the test as skipped instead of failed.
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBBPF REQUIRED IMPORTED_TARGET libbpf)

add_executable(dataplane_test dataplane_test.cpp)
target_link_libraries(dataplane_test PRIVATE
  pb_bpf_skel pb_core pb_abi PkgConfig::LIBBPF GTest::gtest_main)

add_test(NAME dataplane_test COMMAND dataplane_test)
set_tests_properties(dataplane_test PROPERTIES
  LABELS needs-root
  SKIP_REGULAR_EXPRESSION "dataplane tests need root")
