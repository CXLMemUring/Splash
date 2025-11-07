cmake_minimum_required(VERSION 3.15)
project(CXL_Litmus_Tests VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2 -Wall -Wextra")

# Find MPI
find_package(MPI REQUIRED)

# Find OpenMP for thread-based tests
find_package(OpenMP)

# Include directories
include_directories(
    ${CMAKE_SOURCE_DIR}/src
    ${MPI_CXX_INCLUDE_DIRS}
)

# Source files
set(LITMUS_SOURCES
    src/litmus_framework.cpp
    src/hw_coherency_tests.cpp
    src/sw_coherency_tests.cpp
    src/main.cpp
)

# Create executable
add_executable(cxl_litmus_tests ${LITMUS_SOURCES})

# Link libraries
target_link_libraries(cxl_litmus_tests
    ${MPI_CXX_LIBRARIES}
)

if(OpenMP_CXX_FOUND)
    target_link_libraries(cxl_litmus_tests OpenMP::OpenMP_CXX)
endif()

# Add compilation flags for MPI
target_compile_options(cxl_litmus_tests PRIVATE ${MPI_CXX_COMPILE_FLAGS})
target_link_options(cxl_litmus_tests PRIVATE ${MPI_CXX_LINK_FLAGS})

# Installation
install(TARGETS cxl_litmus_tests DESTINATION bin)

# Print build information
message(STATUS "CXL Litmus Tests Configuration:")
message(STATUS "  C++ Compiler: ${CMAKE_CXX_COMPILER}")
message(STATUS "  MPI Found: ${MPI_FOUND}")
message(STATUS "  OpenMP Found: ${OpenMP_CXX_FOUND}")
