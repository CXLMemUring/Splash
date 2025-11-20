# Quick Start Guide

Get started with CXL Cache Coherency Litmus Tests in 5 minutes.

## Prerequisites

You need 2 hosts connected via CXL switch with:
- C++17 compiler (g++ 7+ or clang++ 5+)
- CMake 3.15+
- MPI (OpenMPI, MPICH, or Intel MPI)
- x86-64 architecture

## Installation

### 1. Install Dependencies

**Ubuntu/Debian:**
```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake openmpi-bin libopenmpi-dev
```

**RHEL/CentOS/Rocky:**
```bash
sudo yum install -y gcc-c++ cmake openmpi openmpi-devel
module load mpi/openmpi-x86_64
```

**With Intel MPI:**
```bash
# Download from Intel website, then:
source /opt/intel/oneapi/setvars.sh
```

### 2. Build the Tests

```bash
# Clone or download the project
cd splash

# Build using the convenience script
./build_and_run.sh --build-only

# Or build manually
mkdir build && cd build
cmake ..
make -j$(nproc)
```

### 3. Configure Hosts

Create a hostfile with your 2 hosts:

```bash
# Copy the example
cp hosts.txt.example hosts.txt

# Edit with your hostnames
cat > hosts.txt << EOF
node1 slots=1
node2 slots=1
EOF
```

**Important**: Ensure passwordless SSH is set up between hosts:
```bash
ssh-keygen -t rsa -N "" -f ~/.ssh/id_rsa
ssh-copy-id node2  # Run from node1
ssh-copy-id node1  # Run from node2
```

### 4. Run the Tests

**Run all tests:**
```bash
./build_and_run.sh -f hosts.txt
```

**Run only hardware tests (faster):**
```bash
./build_and_run.sh -f hosts.txt -t hw
```

**Run with more iterations (more thorough):**
```bash
./build_and_run.sh -f hosts.txt -i 10000000
```

**Manual run:**
```bash
mpirun -np 2 --hostfile hosts.txt ./build/cxl_litmus_tests
```

## Understanding Results

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

### What to Look For

- **PASSED (0 violations)**: Coherency is working correctly ✓
- **FAILED (violations > 0)**: Coherency issues detected ✗

### If Tests Fail

1. **Check CXL Configuration**
   ```bash
   # Verify CXL devices are visible
   lspci | grep -i cxl

   # Check memory mapping
   numactl --hardware
   ```

2. **System Tuning**
   ```bash
   # Set performance mode
   sudo cpupower frequency-set -g performance

   # Disable transparent huge pages
   echo never | sudo tee /sys/kernel/mm/transparent_hugepage/enabled
   ```

3. **MPI Configuration**
   ```bash
   # Test basic MPI connectivity
   mpirun -np 2 --hostfile hosts.txt hostname

   # Check MPI version
   mpirun --version
   ```

## Common Issues

### "Could NOT find MPI"
```bash
# Install MPI
sudo apt-get install openmpi-bin libopenmpi-dev

# Or for RHEL/CentOS
sudo yum install openmpi openmpi-devel
module load mpi/openmpi-x86_64
```

### "not enough slots available"
```bash
# Add --oversubscribe flag
mpirun --oversubscribe -np 2 -H node1,node2 ./build/cxl_litmus_tests
```

### "Permission denied" for SSH
```bash
# Set up passwordless SSH
ssh-keygen -t rsa -N "" -f ~/.ssh/id_rsa
ssh-copy-id node2
```

### Slow execution
```bash
# Reduce iterations for quick test
./build_and_run.sh -f hosts.txt -i 10000

# Or run only hardware tests
./build_and_run.sh -f hosts.txt --hw-only -i 100000
```

## Next Steps

- Read the full [README.md](README.md) for detailed documentation
- Understand each test in the "Test Categories" section
- Customize tests for your specific CXL configuration
- Add new tests following the examples in the code

## Performance Tuning

For best results:

1. **Pin MPI processes to specific cores:**
   ```bash
   mpirun -np 2 --bind-to core --map-by socket --hostfile hosts.txt ./build/cxl_litmus_tests
   ```

2. **Use RDMA if available:**
   ```bash
   # With OpenMPI UCX
   mpirun --mca pml ucx -np 2 --hostfile hosts.txt ./build/cxl_litmus_tests
   ```

3. **Set CPU affinity:**
   ```bash
   # Bind to specific CPUs
   mpirun -np 2 --bind-to socket --hostfile hosts.txt ./build/cxl_litmus_tests
   ```

## Testing Checklist

- [ ] MPI is installed and working
- [ ] Passwordless SSH is configured between hosts
- [ ] CXL switch is properly connected
- [ ] Hosts can see CXL memory ranges
- [ ] Tests build without errors
- [ ] Can run simple MPI hello world
- [ ] Litmus tests execute successfully

## Support

If you encounter issues:
1. Check the [README.md](README.md) troubleshooting section
2. Verify your CXL hardware configuration
3. Test with a simple MPI program first
4. Check system logs for hardware errors

## Example Session

```bash
# Complete workflow from scratch
cd splash

# Build
./build_and_run.sh --build-only

# Quick test with few iterations
./build_and_run.sh -f hosts.txt -i 1000

# Full test suite
./build_and_run.sh -f hosts.txt

# Deep test with 10M iterations
./build_and_run.sh -f hosts.txt -i 10000000
```

Happy testing!
