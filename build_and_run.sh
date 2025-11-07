#!/bin/bash

# CXL Litmus Tests - Build and Run Script

set -e  # Exit on error

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Default values
BUILD_DIR="build"
ITERATIONS=1000000
TEST_TYPE="all"
HOSTFILE=""

# Print colored message
print_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

print_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

print_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Print usage
usage() {
    cat << EOF
Usage: $0 [OPTIONS]

Build and run CXL cache coherency litmus tests

OPTIONS:
    -b, --build-only        Only build, don't run tests
    -r, --run-only          Only run tests (skip build)
    -c, --clean             Clean build directory before building
    -i, --iterations N      Number of iterations per test (default: 1000000)
    -t, --test TYPE         Test type: hw, sw, or all (default: all)
    -f, --hostfile FILE     MPI hostfile (required for running tests)
    -h, --help              Show this help message

EXAMPLES:
    # Build and run all tests
    $0 -f hosts.txt

    # Clean build and run only hardware tests
    $0 -c -t hw -f hosts.txt

    # Run with 10 million iterations
    $0 -i 10000000 -f hosts.txt

    # Just build, don't run
    $0 -b

EOF
}

# Check dependencies
check_dependencies() {
    print_info "Checking dependencies..."

    if ! command -v cmake &> /dev/null; then
        print_error "cmake not found. Please install CMake 3.15 or higher."
        exit 1
    fi

    if ! command -v mpirun &> /dev/null && ! command -v mpiexec &> /dev/null; then
        print_error "MPI not found. Please install OpenMPI, MPICH, or Intel MPI."
        exit 1
    fi

    if ! command -v g++ &> /dev/null && ! command -v clang++ &> /dev/null; then
        print_error "C++ compiler not found. Please install g++ or clang++."
        exit 1
    fi

    print_info "All dependencies satisfied"
}

# Build the project
build_project() {
    print_info "Building CXL litmus tests..."

    if [ "$CLEAN_BUILD" = true ]; then
        print_info "Cleaning build directory..."
        rm -rf "$BUILD_DIR"
    fi

    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"

    print_info "Running CMake..."
    cmake .. || {
        print_error "CMake configuration failed"
        exit 1
    }

    print_info "Compiling..."
    make -j$(nproc) || {
        print_error "Build failed"
        exit 1
    }

    cd ..
    print_info "Build completed successfully"
}

# Run the tests
run_tests() {
    print_info "Running CXL litmus tests..."

    if [ ! -f "$BUILD_DIR/cxl_litmus_tests" ]; then
        print_error "Executable not found. Please build first."
        exit 1
    fi

    if [ -z "$HOSTFILE" ]; then
        print_error "Hostfile not specified. Use -f option or set HOSTFILE environment variable."
        echo "Example: $0 -f hosts.txt"
        exit 1
    fi

    if [ ! -f "$HOSTFILE" ]; then
        print_error "Hostfile '$HOSTFILE' not found."
        exit 1
    fi

    # Detect MPI command
    MPI_CMD="mpirun"
    if ! command -v mpirun &> /dev/null; then
        MPI_CMD="mpiexec"
    fi

    # Build command
    CMD="$MPI_CMD -np 2 --hostfile $HOSTFILE $BUILD_DIR/cxl_litmus_tests"

    if [ "$TEST_TYPE" != "all" ]; then
        CMD="$CMD -t $TEST_TYPE"
    fi

    CMD="$CMD -i $ITERATIONS"

    print_info "Executing: $CMD"
    echo ""

    $CMD || {
        print_error "Test execution failed"
        exit 1
    }

    echo ""
    print_info "Tests completed"
}

# Parse command line arguments
BUILD_ONLY=false
RUN_ONLY=false
CLEAN_BUILD=false

while [[ $# -gt 0 ]]; do
    case $1 in
        -b|--build-only)
            BUILD_ONLY=true
            shift
            ;;
        -r|--run-only)
            RUN_ONLY=true
            shift
            ;;
        -c|--clean)
            CLEAN_BUILD=true
            shift
            ;;
        -i|--iterations)
            ITERATIONS="$2"
            shift 2
            ;;
        -t|--test)
            TEST_TYPE="$2"
            shift 2
            ;;
        -f|--hostfile)
            HOSTFILE="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            print_error "Unknown option: $1"
            usage
            exit 1
            ;;
    esac
done

# Main execution
echo "========================================"
echo "  CXL Cache Coherency Litmus Tests"
echo "========================================"
echo ""

check_dependencies

if [ "$RUN_ONLY" = true ]; then
    run_tests
elif [ "$BUILD_ONLY" = true ]; then
    build_project
else
    build_project
    run_tests
fi

print_info "All done!"
