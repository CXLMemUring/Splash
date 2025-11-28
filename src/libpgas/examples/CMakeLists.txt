# Examples for libpgas

# Copy config files
configure_file(${CMAKE_SOURCE_DIR}/config/node0.conf ${CMAKE_CURRENT_BINARY_DIR}/node0.conf COPYONLY)
configure_file(${CMAKE_SOURCE_DIR}/config/node1.conf ${CMAKE_CURRENT_BINARY_DIR}/node1.conf COPYONLY)

# Simple workload example
add_executable(simple_workload simple_workload.c)
target_link_libraries(simple_workload pgas)

# Array workload example
add_executable(array_workload array_workload.c)
target_link_libraries(array_workload pgas)
