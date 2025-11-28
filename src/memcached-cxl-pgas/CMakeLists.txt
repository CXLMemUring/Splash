cmake_minimum_required(VERSION 3.15)
project(memcached_cxl_pgas VERSION 1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O2 -Wall -Wextra -pthread")

# Options
option(BUILD_TESTS "Build test programs" ON)
option(ENABLE_NUMA "Enable NUMA support" ON)
option(USE_SYSTEM_PGAS "Use system-installed libpgas" ON)

# Find required packages
find_package(Threads REQUIRED)

# Find NUMA library (optional)
if(ENABLE_NUMA)
    find_library(NUMA_LIBRARY numa)
    find_path(NUMA_INCLUDE_DIR numa.h)
    if(NUMA_LIBRARY AND NUMA_INCLUDE_DIR)
        set(HAVE_NUMA TRUE)
        message(STATUS "NUMA support enabled")
    else()
        set(HAVE_NUMA FALSE)
        message(STATUS "NUMA library not found, NUMA support disabled")
    endif()
endif()

# Find libpgas (installed or build in-tree)
if(USE_SYSTEM_PGAS)
    find_package(pgas QUIET CONFIG)
    if(NOT pgas_FOUND)
        find_package(PkgConfig)
        if(PkgConfig_FOUND)
            pkg_check_modules(PGAS pgas)
        endif()
    endif()
endif()

if(pgas_FOUND)
    message(STATUS "Using system-installed libpgas")
    set(PGAS_LIBRARIES pgas::pgas_static)
    set(PGAS_INCLUDE_DIRS "")
    set(PGAS_IN_TREE FALSE)
elseif(PGAS_FOUND)
    message(STATUS "Using libpgas from pkg-config")
    set(PGAS_LIBRARIES ${PGAS_LINK_LIBRARIES})
    set(PGAS_IN_TREE FALSE)
else()
    message(STATUS "Building PGAS core in-tree")
    set(PGAS_IN_TREE TRUE)

    # Check for libpgas sources first, fall back to local sources
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../libpgas/include)
        set(PGAS_INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/../libpgas/include)
        set(PGAS_CORE_SOURCES
            ${CMAKE_CURRENT_SOURCE_DIR}/../libpgas/src/pgas.c
            ${CMAKE_CURRENT_SOURCE_DIR}/../libpgas/src/cxl_memory.c
            ${CMAKE_CURRENT_SOURCE_DIR}/../libpgas/src/pgas_workload.c
        )
    else()
        set(PGAS_INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/include)
        set(PGAS_CORE_SOURCES
            src/pgas.c
            src/cxl_memory.c
        )
    endif()

    # Create PGAS core static library
    add_library(pgas_core STATIC ${PGAS_CORE_SOURCES})
    target_include_directories(pgas_core PUBLIC ${PGAS_INCLUDE_DIRS})
    target_link_libraries(pgas_core Threads::Threads m)

    if(HAVE_NUMA)
        target_compile_definitions(pgas_core PUBLIC HAVE_NUMA)
        target_link_libraries(pgas_core ${NUMA_LIBRARY})
    endif()

    set(PGAS_LIBRARIES pgas_core)
endif()

# Include directories
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${PGAS_INCLUDE_DIRS}
)

# Main executable sources
set(MAIN_SOURCES
    src/memcached_interceptor.c
    src/main.c
)

# Create main executable
add_executable(memcached_cxl_pgas ${MAIN_SOURCES})
target_link_libraries(memcached_cxl_pgas
    ${PGAS_LIBRARIES}
    Threads::Threads
    m
)

if(HAVE_NUMA AND PGAS_IN_TREE)
    target_compile_definitions(memcached_cxl_pgas PRIVATE HAVE_NUMA)
    target_link_libraries(memcached_cxl_pgas ${NUMA_LIBRARY})
endif()

# BPF compilation (requires clang and bpftool)
find_program(CLANG clang PATHS /usr/bin NO_DEFAULT_PATH)
find_program(BPFTOOL bpftool)

if(CLANG AND BPFTOOL)
    set(BPF_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/bpf/memcached_uprobe.bpf.c)
    set(BPF_OUTPUT ${CMAKE_BINARY_DIR}/memcached_uprobe.bpf.o)

    # Get vmlinux and libbpf include paths
    set(VMLINUX_DIR ${CMAKE_SOURCE_DIR}/lib/cxltime/third_party/vmlinux/x86)
    set(LIBBPF_INCLUDE ${CMAKE_SOURCE_DIR}/lib/cxltime/third_party/libbpf/include)
    set(LIBBPF_SRC ${CMAKE_SOURCE_DIR}/lib/cxltime/third_party/libbpf/src)

    add_custom_command(
        OUTPUT ${BPF_OUTPUT}
        COMMAND ${CLANG} -target bpf -D__TARGET_ARCH_x86 -O2 -g
            -I${VMLINUX_DIR}
            -I${LIBBPF_INCLUDE}
            -I${LIBBPF_SRC}
            -c ${BPF_SOURCE} -o ${BPF_OUTPUT}
        DEPENDS ${BPF_SOURCE}
        COMMENT "Compiling BPF program"
    )

    add_custom_target(bpf_programs DEPENDS ${BPF_OUTPUT})
    add_dependencies(memcached_cxl_pgas bpf_programs)

    message(STATUS "BPF compilation enabled")
else()
    message(STATUS "BPF compilation disabled (clang or bpftool not found)")
endif()

# Installation
install(TARGETS memcached_cxl_pgas DESTINATION bin)
install(FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/config/nodes.conf.example
    DESTINATION etc/memcached-cxl-pgas
)

# ============================================
# Tests
# ============================================
if(BUILD_TESTS)
    enable_testing()

    # PGAS Self-Loop Test
    add_executable(pgas_selfloop_test tests/pgas_selfloop_test.c)
    target_link_libraries(pgas_selfloop_test
        ${PGAS_LIBRARIES}
        Threads::Threads
        m
    )

    if(HAVE_NUMA AND PGAS_IN_TREE)
        target_compile_definitions(pgas_selfloop_test PRIVATE HAVE_NUMA)
        target_link_libraries(pgas_selfloop_test ${NUMA_LIBRARY})
    endif()

    # Register test with CTest
    add_test(NAME pgas_selfloop_test
             COMMAND pgas_selfloop_test -i 10000 -m 16)

    # PGAS Two-Node Test
    add_executable(pgas_two_node_test tests/pgas_two_node_test.c)
    target_link_libraries(pgas_two_node_test
        ${PGAS_LIBRARIES}
        Threads::Threads
        m
    )

    if(HAVE_NUMA AND PGAS_IN_TREE)
        target_compile_definitions(pgas_two_node_test PRIVATE HAVE_NUMA)
        target_link_libraries(pgas_two_node_test ${NUMA_LIBRARY})
    endif()

    # Copy config files for tests
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/config/node0.conf
                   ${CMAKE_CURRENT_BINARY_DIR}/config/node0.conf COPYONLY)
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/config/node1.conf
                   ${CMAKE_CURRENT_BINARY_DIR}/config/node1.conf COPYONLY)

    # Install test binaries
    install(TARGETS pgas_selfloop_test pgas_two_node_test DESTINATION bin)

    message(STATUS "Tests enabled")
endif()

# Print configuration
message(STATUS "")
message(STATUS "Memcached CXL PGAS Configuration:")
message(STATUS "  C Compiler: ${CMAKE_C_COMPILER}")
message(STATUS "  Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "  NUMA support: ${HAVE_NUMA}")
message(STATUS "  Tests: ${BUILD_TESTS}")
message(STATUS "  PGAS in-tree: ${PGAS_IN_TREE}")
message(STATUS "")
