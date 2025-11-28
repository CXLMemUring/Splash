cmake_minimum_required(VERSION 3.15)
project(gapbs_cxl_pgas VERSION 1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -Wall -Wextra -fopenmp")

# Find OpenMP
find_package(OpenMP REQUIRED)

# Find libpgas (installed or in-tree)
find_package(pgas QUIET CONFIG)
if(NOT pgas_FOUND)
    # Try to find via pkg-config
    find_package(PkgConfig)
    if(PkgConfig_FOUND)
        pkg_check_modules(PGAS pgas)
    endif()

    if(NOT PGAS_FOUND)
        # Fallback: use in-tree libpgas or memcached-cxl-pgas sources
        message(STATUS "Using in-tree PGAS sources")
        set(PGAS_IN_TREE TRUE)

        # Check for libpgas first
        if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../libpgas/include)
            set(PGAS_INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/../libpgas/include)
            set(PGAS_SOURCES
                ${CMAKE_CURRENT_SOURCE_DIR}/../libpgas/src/pgas.c
                ${CMAKE_CURRENT_SOURCE_DIR}/../libpgas/src/cxl_memory.c
                ${CMAKE_CURRENT_SOURCE_DIR}/../libpgas/src/pgas_workload.c
            )
        else()
            set(PGAS_INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/../memcached-cxl-pgas/include)
            set(PGAS_SOURCES
                ${CMAKE_CURRENT_SOURCE_DIR}/../memcached-cxl-pgas/src/pgas.c
                ${CMAKE_CURRENT_SOURCE_DIR}/../memcached-cxl-pgas/src/cxl_memory.c
            )
        endif()
    else()
        set(PGAS_IN_TREE FALSE)
    endif()
else()
    message(STATUS "Found installed libpgas")
    set(PGAS_IN_TREE FALSE)
    set(PGAS_INCLUDE_DIRS "")  # Will use imported target
endif()

# Include directories
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${PGAS_INCLUDE_DIRS}
)

# Create PGAS library target if using in-tree sources
if(PGAS_IN_TREE AND DEFINED PGAS_SOURCES)
    add_library(pgas_intree STATIC ${PGAS_SOURCES})
    target_include_directories(pgas_intree PUBLIC ${PGAS_INCLUDE_DIRS})
    target_link_libraries(pgas_intree PUBLIC pthread)
    set(PGAS_LIBRARIES pgas_intree)
elseif(pgas_FOUND)
    set(PGAS_LIBRARIES pgas::pgas_static)
else()
    set(PGAS_LIBRARIES ${PGAS_LINK_LIBRARIES})
endif()

# Main executable
add_executable(gapbs_pgas src/main.cpp)
target_link_libraries(gapbs_pgas ${PGAS_LIBRARIES} OpenMP::OpenMP_CXX pthread m)

# Individual algorithm executables
add_executable(bfs_pgas src/main.cpp)
target_compile_definitions(bfs_pgas PRIVATE DEFAULT_ALG="bfs")
target_link_libraries(bfs_pgas ${PGAS_LIBRARIES} OpenMP::OpenMP_CXX pthread m)

add_executable(pr_pgas src/main.cpp)
target_compile_definitions(pr_pgas PRIVATE DEFAULT_ALG="pr")
target_link_libraries(pr_pgas ${PGAS_LIBRARIES} OpenMP::OpenMP_CXX pthread m)

add_executable(cc_pgas src/main.cpp)
target_compile_definitions(cc_pgas PRIVATE DEFAULT_ALG="cc")
target_link_libraries(cc_pgas ${PGAS_LIBRARIES} OpenMP::OpenMP_CXX pthread m)

add_executable(tc_pgas src/main.cpp)
target_compile_definitions(tc_pgas PRIVATE DEFAULT_ALG="tc")
target_link_libraries(tc_pgas ${PGAS_LIBRARIES} OpenMP::OpenMP_CXX pthread m)

add_executable(sssp_pgas src/main.cpp)
target_compile_definitions(sssp_pgas PRIVATE DEFAULT_ALG="sssp")
target_link_libraries(sssp_pgas ${PGAS_LIBRARIES} OpenMP::OpenMP_CXX pthread m)

# Copy config files
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/config/node0.conf
               ${CMAKE_CURRENT_BINARY_DIR}/node0.conf COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/config/node1.conf
               ${CMAKE_CURRENT_BINARY_DIR}/node1.conf COPYONLY)

# Installation
install(TARGETS gapbs_pgas bfs_pgas pr_pgas cc_pgas tc_pgas sssp_pgas
        DESTINATION bin)

# Print configuration
message(STATUS "")
message(STATUS "GAPBS CXL PGAS Configuration:")
message(STATUS "  C++ Compiler: ${CMAKE_CXX_COMPILER}")
message(STATUS "  OpenMP: ${OpenMP_CXX_FLAGS}")
message(STATUS "  PGAS source: ${PGAS_IN_TREE}")
if(PGAS_IN_TREE)
    message(STATUS "  PGAS include: ${PGAS_INCLUDE_DIRS}")
endif()
message(STATUS "")
