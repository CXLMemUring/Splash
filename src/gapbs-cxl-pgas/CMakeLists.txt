cmake_minimum_required(VERSION 3.15)
project(gapbs_cxl_pgas VERSION 1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -Wall -Wextra -fopenmp")

# Find OpenMP
find_package(OpenMP REQUIRED)

# Include directories
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../memcached-cxl-pgas/include
)

# PGAS library sources (from memcached-cxl-pgas)
set(PGAS_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/../memcached-cxl-pgas/src/pgas.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../memcached-cxl-pgas/src/cxl_memory.c
)

# Main executable
add_executable(gapbs_pgas
    src/main.cpp
    ${PGAS_SOURCES}
)

target_link_libraries(gapbs_pgas
    OpenMP::OpenMP_CXX
    pthread
    m
)

# Individual algorithm executables
add_executable(bfs_pgas src/main.cpp ${PGAS_SOURCES})
target_compile_definitions(bfs_pgas PRIVATE DEFAULT_ALG="bfs")
target_link_libraries(bfs_pgas OpenMP::OpenMP_CXX pthread m)

add_executable(pr_pgas src/main.cpp ${PGAS_SOURCES})
target_compile_definitions(pr_pgas PRIVATE DEFAULT_ALG="pr")
target_link_libraries(pr_pgas OpenMP::OpenMP_CXX pthread m)

add_executable(cc_pgas src/main.cpp ${PGAS_SOURCES})
target_compile_definitions(cc_pgas PRIVATE DEFAULT_ALG="cc")
target_link_libraries(cc_pgas OpenMP::OpenMP_CXX pthread m)

add_executable(tc_pgas src/main.cpp ${PGAS_SOURCES})
target_compile_definitions(tc_pgas PRIVATE DEFAULT_ALG="tc")
target_link_libraries(tc_pgas OpenMP::OpenMP_CXX pthread m)

# Installation
install(TARGETS gapbs_pgas bfs_pgas pr_pgas cc_pgas tc_pgas
        DESTINATION bin)

# Print configuration
message(STATUS "GAPBS CXL PGAS Configuration:")
message(STATUS "  C++ Compiler: ${CMAKE_CXX_COMPILER}")
message(STATUS "  OpenMP: ${OpenMP_CXX_FLAGS}")
