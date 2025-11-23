cmake_minimum_required(VERSION 3.15)
project(memcached_cxl_pgas VERSION 1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O2 -Wall -Wextra -pthread")

# Find required packages
find_package(Threads REQUIRED)

# Include directories
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Source files
set(SOURCES
    src/pgas.c
    src/cxl_memory.c
    src/memcached_interceptor.c
    src/main.c
)

# Create executable
add_executable(memcached_cxl_pgas ${SOURCES})

# Link libraries
target_link_libraries(memcached_cxl_pgas
    Threads::Threads
    m
)

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

# Print configuration
message(STATUS "Memcached CXL PGAS Configuration:")
message(STATUS "  C Compiler: ${CMAKE_C_COMPILER}")
message(STATUS "  Build type: ${CMAKE_BUILD_TYPE}")
