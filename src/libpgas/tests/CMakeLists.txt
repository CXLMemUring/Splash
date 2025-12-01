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

# CXL workload benchmark
add_executable(cxl_workload_benchmark cxl_workload_benchmark.c)
target_link_libraries(cxl_workload_benchmark pgas m)

# MCF integration test
add_executable(mcf_integration_test mcf_integration_test.c)
target_link_libraries(mcf_integration_test pgas m)

# LLAMA integration test
add_executable(llama_integration_test llama_integration_test.c)
target_link_libraries(llama_integration_test pgas m)

# GROMACS integration test
add_executable(gromacs_integration_test gromacs_integration_test.c)
target_link_libraries(gromacs_integration_test pgas m)

# VSAG integration test
add_executable(vsag_integration_test vsag_integration_test.c)
target_link_libraries(vsag_integration_test pgas m)

# VSAG PGAS benchmark
add_executable(vsag_pgas_benchmark vsag_pgas_benchmark.c)
target_link_libraries(vsag_pgas_benchmark pgas m)

# Add tests
add_test(NAME selfloop_test COMMAND pgas_selfloop_test -c ${CMAKE_CURRENT_BINARY_DIR}/node0.conf -i 100)
