# pbctl: CLI for the daemon's unix-socket JSON API. Portable (macOS too): it
# needs only POSIX sockets and nlohmann_json, no libbpf.
find_package(nlohmann_json REQUIRED)
add_executable(pbctl main.cpp client.cpp format.cpp)
target_link_libraries(pbctl PRIVATE pb_core nlohmann_json::nlohmann_json)
install(TARGETS pbctl RUNTIME DESTINATION bin)
