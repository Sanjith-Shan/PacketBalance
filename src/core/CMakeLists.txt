# libpb_core: everything the daemon, pbctl, the tools and the tests share and
# that does not need a kernel. Globbed so the core and daemon work can add
# files without editing this list.
file(GLOB PB_CORE_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
add_library(pb_core STATIC ${PB_CORE_SOURCES})
target_link_libraries(pb_core PUBLIC pb_abi)
target_include_directories(pb_core PUBLIC ${CMAKE_SOURCE_DIR}/include)

find_package(yaml-cpp QUIET)
if(yaml-cpp_FOUND)
  target_link_libraries(pb_core PUBLIC yaml-cpp)
  target_compile_definitions(pb_core PUBLIC PB_HAVE_YAML=1)
endif()
find_package(nlohmann_json QUIET)
if(nlohmann_json_FOUND)
  target_link_libraries(pb_core PUBLIC nlohmann_json::nlohmann_json)
endif()
