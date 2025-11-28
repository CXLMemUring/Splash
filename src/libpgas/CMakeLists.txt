cmake_minimum_required(VERSION 3.10)
project(libpgas VERSION 1.0.0 LANGUAGES C)

# Options
option(PGAS_BUILD_SHARED "Build shared library" ON)
option(PGAS_BUILD_STATIC "Build static library" ON)
option(PGAS_BUILD_TESTS "Build tests" ON)
option(PGAS_BUILD_EXAMPLES "Build examples" ON)

# C standard
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Compiler flags
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -O2")
set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -g -O0 -DDEBUG")
set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -O3 -DNDEBUG")

# Find required packages
find_package(Threads REQUIRED)

# Include directories
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

# Source files
set(PGAS_SOURCES
    src/pgas.c
    src/cxl_memory.c
    src/pgas_workload.c
)

# Header files (for installation)
set(PGAS_HEADERS
    include/pgas.h
    include/cxl_memory.h
    include/pgas_workload.h
)

# Build static library
if(PGAS_BUILD_STATIC)
    add_library(pgas_static STATIC ${PGAS_SOURCES})
    target_link_libraries(pgas_static PUBLIC Threads::Threads)
    target_include_directories(pgas_static PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    )
    set_target_properties(pgas_static PROPERTIES
        OUTPUT_NAME pgas
        POSITION_INDEPENDENT_CODE ON
    )
endif()

# Build shared library
if(PGAS_BUILD_SHARED)
    add_library(pgas_shared SHARED ${PGAS_SOURCES})
    target_link_libraries(pgas_shared PUBLIC Threads::Threads)
    target_include_directories(pgas_shared PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    )
    set_target_properties(pgas_shared PROPERTIES
        OUTPUT_NAME pgas
        VERSION ${PROJECT_VERSION}
        SOVERSION 1
    )
endif()

# Create alias targets for cleaner CMake
if(PGAS_BUILD_STATIC)
    add_library(pgas::static ALIAS pgas_static)
endif()
if(PGAS_BUILD_SHARED)
    add_library(pgas::shared ALIAS pgas_shared)
endif()

# Default pgas target (prefer static)
if(PGAS_BUILD_STATIC)
    add_library(pgas ALIAS pgas_static)
elseif(PGAS_BUILD_SHARED)
    add_library(pgas ALIAS pgas_shared)
endif()

# Installation
include(GNUInstallDirs)

# Install libraries
if(PGAS_BUILD_STATIC)
    install(TARGETS pgas_static
        EXPORT pgasTargets
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )
endif()

if(PGAS_BUILD_SHARED)
    install(TARGETS pgas_shared
        EXPORT pgasTargets
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )
endif()

# Install headers
install(FILES ${PGAS_HEADERS}
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/pgas
)

# Install config files
install(DIRECTORY config/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/pgas/config
    FILES_MATCHING PATTERN "*.conf"
)

# Generate and install CMake config files
install(EXPORT pgasTargets
    FILE pgasTargets.cmake
    NAMESPACE pgas::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/pgas
)

include(CMakePackageConfigHelpers)
configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/pgasConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/pgasConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/pgas
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/pgasConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/pgasConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/pgasConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/pgas
)

# pkg-config file
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/pgas.pc.in
    ${CMAKE_CURRENT_BINARY_DIR}/pgas.pc
    @ONLY
)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/pgas.pc
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
)

# Tests
if(PGAS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Examples
if(PGAS_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# Print summary
message(STATUS "")
message(STATUS "PGAS Library Configuration:")
message(STATUS "  Version: ${PROJECT_VERSION}")
message(STATUS "  Build shared: ${PGAS_BUILD_SHARED}")
message(STATUS "  Build static: ${PGAS_BUILD_STATIC}")
message(STATUS "  Build tests: ${PGAS_BUILD_TESTS}")
message(STATUS "  Build examples: ${PGAS_BUILD_EXAMPLES}")
message(STATUS "  Install prefix: ${CMAKE_INSTALL_PREFIX}")
message(STATUS "")
