# Tests for libpgas

# Copy test config files
configure_file(${CMAKE_SOURCE_DIR}/config/node0.conf ${CMAKE_CURRENT_BINARY_DIR}/node0.conf COPYONLY)
configure_file(${CMAKE_SOURCE_DIR}/config/node1.conf ${CMAKE_CURRENT_BINARY_DIR}/node1.conf COPYONLY)

# Self-loop test
add_executable(pgas_selfloop_test pgas_selfloop_test.c)
target_link_libraries(pgas_selfloop_test pgas)

# Two-node test
add_executable(pgas_two_node_test pgas_two_node_test.c)
target_link_libraries(pgas_two_node_test pgas)

# Tuning profile test
add_executable(tuning_test tuning_test.c)
target_link_libraries(tuning_test pgas)

# Profile benchmark
add_executable(profile_benchmark profile_benchmark.c)
target_link_libraries(profile_benchmark pgas)

# Simple profile test (no multi-node required)
add_executable(simple_profile_test simple_profile_test.c)
target_link_libraries(simple_profile_test pgas)

# Workload benchmark (MCF, LLAMA, GROMACS)
add_executable(workload_benchmark workload_benchmark.c)
target_link_libraries(workload_benchmark pgas m)

# Add tests
add_test(NAME selfloop_test COMMAND pgas_selfloop_test -c ${CMAKE_CURRENT_BINARY_DIR}/node0.conf -i 100)
