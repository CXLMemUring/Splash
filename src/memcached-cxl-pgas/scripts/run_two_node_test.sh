#!/bin/bash
#
# PGAS Two-Node Self-Loop Test Launcher
#
# Runs two PGAS processes on the same machine to test inter-process
# communication via CXL memory.
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${PROJECT_DIR}/build"
CONFIG_DIR="${PROJECT_DIR}/config"

# Test binary
TEST_BIN="${BUILD_DIR}/pgas_two_node_test"

# Configuration files
NODE0_CONF="${CONFIG_DIR}/node0.conf"
NODE1_CONF="${CONFIG_DIR}/node1.conf"

# Default parameters
ITERATIONS=1000
MSG_SIZE=64
TEST_NAME="all"
VERBOSE=""

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

usage() {
    echo "Usage: $0 [options]"
    echo ""
    echo "PGAS Two-Node Self-Loop Test Launcher"
    echo ""
    echo "Options:"
    echo "  -i, --iterations N   Number of iterations (default: $ITERATIONS)"
    echo "  -s, --size BYTES     Message size for bulk test (default: $MSG_SIZE)"
    echo "  -t, --test TEST      Run specific test: ping|atomic|bulk|msg|all (default: all)"
    echo "  -v, --verbose        Enable verbose output"
    echo "  -b, --build          Build before running"
    echo "  -h, --help           Show this help"
    echo ""
    echo "Tests:"
    echo "  ping   - Ping-pong latency test"
    echo "  atomic - Remote atomic operations test"
    echo "  bulk   - Bulk data transfer test"
    echo "  msg    - Message passing test"
    echo "  all    - Run all tests"
}

# Parse arguments
BUILD=0
while [[ $# -gt 0 ]]; do
    case $1 in
        -i|--iterations)
            ITERATIONS="$2"
            shift 2
            ;;
        -s|--size)
            MSG_SIZE="$2"
            shift 2
            ;;
        -t|--test)
            TEST_NAME="$2"
            shift 2
            ;;
        -v|--verbose)
            VERBOSE="-v"
            shift
            ;;
        -b|--build)
            BUILD=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            usage
            exit 1
            ;;
    esac
done

# Build if requested
if [[ $BUILD -eq 1 ]]; then
    echo -e "${YELLOW}Building...${NC}"
    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"
    cmake .. -DCMAKE_BUILD_TYPE=Release
    make -j$(nproc)
    cd "$SCRIPT_DIR"
fi

# Check if binary exists
if [[ ! -f "$TEST_BIN" ]]; then
    echo -e "${RED}Error: Test binary not found: $TEST_BIN${NC}"
    echo "Run with -b to build first, or build manually:"
    echo "  cd $BUILD_DIR && cmake .. && make"
    exit 1
fi

# Check if config files exist
if [[ ! -f "$NODE0_CONF" ]] || [[ ! -f "$NODE1_CONF" ]]; then
    echo -e "${RED}Error: Configuration files not found${NC}"
    echo "Expected: $NODE0_CONF and $NODE1_CONF"
    exit 1
fi

echo "============================================"
echo "  PGAS Two-Node Self-Loop Test"
echo "============================================"
echo ""
echo "Configuration:"
echo "  Iterations: $ITERATIONS"
echo "  Message size: $MSG_SIZE bytes"
echo "  Test: $TEST_NAME"
echo ""

# Create log directory
LOG_DIR="/tmp/pgas_test_$$"
mkdir -p "$LOG_DIR"

# Cleanup function
cleanup() {
    echo ""
    echo -e "${YELLOW}Cleaning up...${NC}"
    # Kill any remaining processes
    if [[ -n "$NODE0_PID" ]] && kill -0 "$NODE0_PID" 2>/dev/null; then
        kill "$NODE0_PID" 2>/dev/null || true
    fi
    if [[ -n "$NODE1_PID" ]] && kill -0 "$NODE1_PID" 2>/dev/null; then
        kill "$NODE1_PID" 2>/dev/null || true
    fi
    wait 2>/dev/null || true
}
trap cleanup EXIT

# Start Node 0
echo -e "${GREEN}Starting Node 0...${NC}"
"$TEST_BIN" -c "$NODE0_CONF" -i "$ITERATIONS" -s "$MSG_SIZE" -t "$TEST_NAME" $VERBOSE \
    > "$LOG_DIR/node0.log" 2>&1 &
NODE0_PID=$!
echo "  Node 0 PID: $NODE0_PID"

# Small delay to let Node 0 initialize
sleep 0.5

# Start Node 1
echo -e "${GREEN}Starting Node 1...${NC}"
"$TEST_BIN" -c "$NODE1_CONF" -i "$ITERATIONS" -s "$MSG_SIZE" -t "$TEST_NAME" $VERBOSE \
    > "$LOG_DIR/node1.log" 2>&1 &
NODE1_PID=$!
echo "  Node 1 PID: $NODE1_PID"

echo ""
echo -e "${YELLOW}Waiting for tests to complete...${NC}"
echo "(Press Ctrl+C to abort)"
echo ""

# Wait for both processes
NODE0_EXIT=0
NODE1_EXIT=0

wait $NODE0_PID || NODE0_EXIT=$?
wait $NODE1_PID || NODE1_EXIT=$?

# Print results
echo "============================================"
echo "  Results"
echo "============================================"
echo ""

echo -e "${GREEN}--- Node 0 Output ---${NC}"
cat "$LOG_DIR/node0.log"
echo ""

echo -e "${GREEN}--- Node 1 Output ---${NC}"
cat "$LOG_DIR/node1.log"
echo ""

echo "============================================"
echo "  Summary"
echo "============================================"

if [[ $NODE0_EXIT -eq 0 ]] && [[ $NODE1_EXIT -eq 0 ]]; then
    echo -e "${GREEN}All tests PASSED${NC}"
    EXIT_CODE=0
else
    echo -e "${RED}Tests FAILED${NC}"
    echo "  Node 0 exit code: $NODE0_EXIT"
    echo "  Node 1 exit code: $NODE1_EXIT"
    EXIT_CODE=1
fi

# Cleanup logs
rm -rf "$LOG_DIR"

exit $EXIT_CODE
