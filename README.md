# CXL Cache Coherency Litmus Tests

A comprehensive test suite for validating hardware and software cache coherency in CXL (Compute Express Link) systems across multiple hosts using MPI.

## Overview

This test suite implements classic litmus tests from memory consistency literature, adapted for testing CXL cache coherency protocols. It tests both:

1. **Hardware Cache Coherency** (HDM-H mode): Tests built-in hardware coherency mechanisms in CXL.cache
2. **Software Cache Coherency** (HDM-D mode): Tests software-managed coherency with explicit cache flushes

## Features

- **22 Litmus Tests** covering both hardware and software coherency scenarios
- **MPI-based** distributed execution across 2 hosts
- **Configurable** test iterations for statistical significance
- **CXL-specific** tests for cache line sharing and memory ordering
- **Detailed reporting** with violation rates and latency measurements

## Requirements

- C++17 compatible compiler (g++ or clang++)
- CMake 3.15 or higher
- MPI implementation (OpenMPI, MPICH, or Intel MPI)
- x86-64 architecture (uses CLFLUSH instructions)
- 2 hosts with CXL switch connectivity

## Building

```bash
mkdir build
cd build
cmake ..
make -j$(nproc)
```

## Running Tests

### Basic Usage

Run all tests with default settings (1,000,000 iterations):

```bash
mpirun -np 2 --host host1,host2 ./cxl_litmus_tests
```

### Run Only Hardware Tests

```bash
mpirun -np 2 --host host1,host2 ./cxl_litmus_tests --hw-only
```

### Run Only Software Tests

```bash
mpirun -np 2 --host host1,host2 ./cxl_litmus_tests --sw-only
```

### Custom Iterations

```bash
mpirun -np 2 --host host1,host2 ./cxl_litmus_tests -i 10000000
```

### Options

```
-h, --help              Show help message
-i, --iterations N      Number of iterations per test (default: 1000000)
-t, --test TYPE         Run specific test type: hw, sw, or all (default: all)
--hw-only               Run only hardware coherency tests
--sw-only               Run only software coherency tests
```

## Test Categories

### Hardware Cache Coherency Tests

These tests validate hardware-enforced cache coherency in CXL systems:

1. **Store Buffer (SB)**: Tests if stores can be reordered or buffered
   - Pattern: `P0: x=1, r0=y | P1: y=1, r1=x`
   - Forbidden: `r0=0, r1=0`

2. **Load Buffer (LB)**: Tests if loads can be reordered before stores
   - Pattern: `P0: r0=x, y=1 | P1: r1=y, x=1`
   - Forbidden: `r0=1, r1=1`

3. **Message Passing (MP)**: Validates message passing semantics
   - Pattern: `P0: x=1, y=1 | P1: r0=y, r1=x`
   - Forbidden: `r0=1, r1=0` (flag seen but not data)

4. **Write Causality (WRC)**: Tests causality of writes across processors
   - Validates that causal ordering is preserved

5. **IRIW**: Tests if writes are observed in consistent order
   - Validates write ordering visibility

6. **Read-Read Coherence (CoRR)**: Validates consecutive reads see values in order
   - Pattern: `P0: x=1, x=2 | P1: r0=x, r1=x`
   - Forbidden: `r0=2, r1=1`

7. **Write-Write Coherence (CoWW)**: Tests global write ordering

8. **Read-Write Coherence (CoRW)**: Tests read-after-write coherence

9. **CXL Cache Line Sharing**: Tests cache line sharing across CXL.cache protocol

10. **CXL.mem Ordering**: Tests memory ordering guarantees with CXL.mem transactions

### Software Cache Coherency Tests

These tests validate software-managed coherency protocols:

1. **Dekker's Algorithm**: Classic mutual exclusion test
   - Tests if both processes can enter critical section simultaneously

2. **Peterson's Algorithm**: Another mutual exclusion protocol
   - Validates lock correctness with memory ordering

3. **Lamport's Bakery**: FIFO mutual exclusion
   - Tests ticket-based locking

4. **Software Barrier**: Validates software barrier synchronization

5. **Producer-Consumer**: Tests single-producer single-consumer queue

6. **RCU Pattern**: Tests Read-Copy-Update pattern for lock-free reads

7. **Seqlock**: Tests sequence lock with writer priority

8. **Test-and-Set Lock**: Validates atomic test-and-set spinlock

9. **CAS Lock**: Tests compare-and-swap based locking

10. **Software Cache Flush**: Tests explicit cache flushes for HDM-D mode
    - Uses CLFLUSHOPT instructions

11. **Explicit Memory Fence**: Tests mfence/sfence/lfence effectiveness

12. **Double-Checked Locking**: Detects broken double-checked locking patterns

## Understanding Results

### Test Output

Each test reports:
- **Iterations**: Number of test executions
- **Violations**: Number of times forbidden outcome occurred
- **Violation Rate**: Percentage of iterations that violated coherency
- **Avg Latency**: Average execution time per iteration
- **Status**: PASSED (0 violations) or FAILED (violations detected)

### Expected Results

#### Correct Coherency Implementation
- All hardware tests should **PASS** with 0 violations
- Most software tests should **PASS** (some may have rare violations due to implementation)

#### Coherency Issues Detected
- **Non-zero violations** indicate:
  - Hardware coherency protocol issues in CXL switch
  - Memory ordering problems in CPU/CXL interaction
  - Software barrier implementation bugs
  - Cache flush ineffectiveness

### Example Output

```
Test: Store Buffer (SB)
Description: Tests store buffering - detects if stores can be reordered
Iterations: 1000000
Violations: 0
Violation Rate: 0.000000%
Avg Latency: 2.345 μs
Status: PASSED ✓
```

## CXL-Specific Considerations

### Hardware Coherent (HDM-H) Mode

In HDM-H mode, the CXL device participates in hardware cache coherency:
- Cache lines are tracked by host CPU cache coherency protocol
- CXL.cache protocol handles snoop messages
- Hardware guarantees coherency across all caches

Tests like **CXL Cache Line Sharing** specifically validate HDM-H operation.

### Device Coherent (HDM-D) Mode

In HDM-D mode, coherency is software-managed:
- Explicit cache flushes (CLFLUSH/CLFLUSHOPT) required
- Memory barriers (mfence/sfence/lfence) needed for ordering
- Software responsible for maintaining consistency

Tests like **Software Cache Flush** and **Explicit Fence** validate HDM-D operation.

## Memory Ordering Levels

The tests use different C++ memory orderings to test various coherency levels:

- `memory_order_relaxed`: No ordering guarantees
- `memory_order_acquire`: Prevents reordering of subsequent loads/stores
- `memory_order_release`: Prevents reordering of prior loads/stores
- `memory_order_acq_rel`: Both acquire and release semantics
- `memory_order_seq_cst`: Sequentially consistent ordering (strongest)

## Troubleshooting

### High Violation Rates

If you see high violation rates:

1. **Verify CXL Configuration**
   - Check if CXL switch firmware is up to date
   - Verify HDM ranges are configured correctly
   - Ensure proper NUMA binding

2. **Check System Settings**
   - Disable CPU frequency scaling: `cpupower frequency-set -g performance`
   - Disable transparent huge pages
   - Pin MPI processes to specific cores

3. **Network/MPI Issues**
   - Ensure low-latency interconnect (not just TCP/IP)
   - Use RDMA-capable MPI if available
   - Check for network congestion

### Build Issues

```bash
# If MPI is not found
export PATH=/path/to/mpi/bin:$PATH
export LD_LIBRARY_PATH=/path/to/mpi/lib:$LD_LIBRARY_PATH

# If using Intel MPI
source /opt/intel/oneapi/setvars.sh
```

### Runtime Issues

```bash
# If you get "not enough slots available"
mpirun --oversubscribe -np 2 ./cxl_litmus_tests

# If you need to specify hostfile
mpirun -np 2 --hostfile hosts.txt ./cxl_litmus_tests
```

## Architecture

```
src/
├── litmus_framework.h/cpp     # Base framework for litmus tests
├── hw_coherency_tests.h/cpp   # Hardware coherency test implementations
├── sw_coherency_tests.h/cpp   # Software coherency test implementations
└── main.cpp                   # Test runner and CLI
```

### Adding New Tests

To add a new test:

1. Create a new class inheriting from `LitmusTest`
2. Implement three methods:
   - `run_process0()`: Code for process 0
   - `run_process1()`: Code for process 1
   - `check_violation()`: Violation detection logic
3. Add the test to the appropriate category in `main.cpp`

Example:

```cpp
class MyTest : public LitmusTest {
public:
    using LitmusTest::LitmusTest;
    std::string get_name() const override { return "My Test"; }
    std::string get_description() const override { return "Tests X"; }

    void run_process0() override {
        // Process 0 code
    }

    void run_process1() override {
        // Process 1 code
    }

    bool check_violation() override {
        // Return true if coherency violated
        return false;
    }
};
```

## References

1. **Memory Consistency Models**: Adve & Gharachorloo, "Shared Memory Consistency Models: A Tutorial"
2. **Litmus Tests**: Alglave et al., "Litmus: Running Tests against Hardware"
3. **CXL Specification**: Compute Express Link Specification 2.0/3.0
4. **x86-64 Memory Model**: Intel Software Developer's Manual, Volume 3A

## License

This test suite is provided for research and validation purposes.

## Contributing

When adding new tests:
- Follow existing code style
- Add clear documentation
- Include references to academic papers if applicable
- Test on actual CXL hardware

## Authors

Generated for CXL cache coherency validation and testing.
