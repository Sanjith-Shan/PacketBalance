# Experiment 5: one million synthetic flows through the maglev and modulo rings.
add_executable(hashquality hashquality.cpp)
target_link_libraries(hashquality PRIVATE pb_core)
