# Quick DAX/mmap Configuration Guide

## The Error You Saw

```
Check failed: resource != "SS" Backend mmap requires --cxl_memory_resource to point at a file or device
```

This happens when `CXL_BACKEND` is set to "mmap" or "dax" but `CXL_MEMORY_RESOURCE` is still "SS" (shared memory).

## Solution: Always Set BOTH Variables Together

### Option 1: Use DAX Device (Best Performance)

```bash
export CXL_BACKEND="dax"
export CXL_MEMORY_RESOURCE="/dev/dax0.0"

# Then run
./scripts/run_tpcc.sh ./results
```

### Option 2: Use File-based mmap (For Testing)

```bash
export CXL_BACKEND="mmap"
export CXL_MEMORY_RESOURCE="/tmp/cxl_memory"

# Then run
./scripts/run_ycsb.sh ./results
```

### Option 3: Use Config Script (Recommended)

```bash
# Edit scripts/dax_config.sh to set your device/file
# Then source it:
source ./scripts/dax_config.sh

# Run benchmarks
./scripts/run_tpcc.sh ./results
```

### Option 4: Use Default (IVSHMEM Shared Memory)

```bash
# Don't set CXL_BACKEND or CXL_MEMORY_RESOURCE
# Or explicitly unset them:
unset CXL_BACKEND
unset CXL_MEMORY_RESOURCE

# Run with original behavior
./scripts/run_tpcc.sh ./results
```

## Common Configurations

### For Production with Real DAX Hardware

```bash
#!/bin/bash
# production_dax.sh

# Configure DAX mode
export CXL_BACKEND="dax"
export CXL_MEMORY_RESOURCE="/dev/dax0.0"

# Verify device exists
if [ ! -e "$CXL_MEMORY_RESOURCE" ]; then
    echo "Error: DAX device not found: $CXL_MEMORY_RESOURCE"
    echo "Create it with: ndctl create-namespace -m devdax"
    exit 1
fi

# Run benchmark
cd /root/CXLMemSim/workloads/tigon
./scripts/run_tpcc.sh ./results
```

### For Testing Without Real DAX

```bash
#!/bin/bash
# test_mmap.sh

# Create test file
mkdir -p /tmp/cxl_test
touch /tmp/cxl_test/memory

# Configure mmap mode
export CXL_BACKEND="mmap"
export CXL_MEMORY_RESOURCE="/tmp/cxl_test/memory"

# Run benchmark
cd /root/CXLMemSim/workloads/tigon
./scripts/run_ycsb.sh ./results
```

### For QEMU VM Testing

```bash
#!/bin/bash
# qemu_vm.sh

# Inside QEMU VM with CXL device:
# 1. Create DAX namespace
cxl create-region -d decoder0.0 -w 1 mem0
ndctl create-namespace -r region0 -m devdax

# 2. Find the device
ls -l /dev/dax*
# Should show: /dev/dax0.0

# 3. Configure tigon
export CXL_BACKEND="dax"
export CXL_MEMORY_RESOURCE="/dev/dax0.0"

# 4. Run
cd /root/pasha  # or wherever tigon is
./bench_tpcc --logtostderr=1 --id=0 --servers="localhost:18000" \
    --cxl_backend=$CXL_BACKEND \
    --cxl_memory_resource=$CXL_MEMORY_RESOURCE \
    ...other flags...
```

## Verification

After setting the environment variables, verify they're correct:

```bash
echo "Backend: $CXL_BACKEND"
echo "Resource: $CXL_MEMORY_RESOURCE"

# Check resource exists
if [ "$CXL_MEMORY_RESOURCE" != "SS" ]; then
    ls -lh "$CXL_MEMORY_RESOURCE"
fi
```

## Troubleshooting

### Issue: "Backend mmap requires --cxl_memory_resource to point at a file or device"

**Cause**: You set `CXL_BACKEND="mmap"` but didn't set `CXL_MEMORY_RESOURCE`

**Fix**:
```bash
export CXL_MEMORY_RESOURCE="/dev/dax0.0"  # or a file path
```

### Issue: Device not found

**Cause**: DAX device doesn't exist

**Fix**:
```bash
# Check what devices exist
ls -l /dev/dax*
ls -l /dev/pmem*

# Create namespace if needed
ndctl create-namespace -m devdax -e namespace0.0

# Or use a regular file for testing
export CXL_MEMORY_RESOURCE="/tmp/cxl_memory"
touch /tmp/cxl_memory
```

### Issue: Permission denied

**Cause**: Don't have permissions to access the device

**Fix**:
```bash
# Option 1: Fix permissions
sudo chmod 666 /dev/dax0.0

# Option 2: Run as root (with -E to preserve environment)
sudo -E ./scripts/run_tpcc.sh ./results
```

## Valid Combinations

| CXL_BACKEND | CXL_MEMORY_RESOURCE | Result |
|-------------|---------------------|--------|
| (not set) | (not set) | ✅ Uses ivshmem (default) |
| (not set) | "SS" | ✅ Uses ivshmem |
| "mmap" | "/tmp/file" | ✅ Uses mmap with file |
| "dax" | "/dev/dax0.0" | ✅ Uses DAX device |
| "mmap" | "SS" | ❌ ERROR - mmap needs file/device |
| "dax" | "SS" | ❌ ERROR - DAX needs device |
| "mmap" | (not set) | ❌ WARNING - falls back to default |

## Complete Example

```bash
#!/bin/bash
# complete_example.sh

cd /root/CXLMemSim/workloads/tigon

# Method 1: Direct environment variables
export CXL_BACKEND="dax"
export CXL_MEMORY_RESOURCE="/dev/dax0.0"

# Method 2: Use config script (alternatively)
# source ./scripts/dax_config.sh

# Verify setup
./scripts/verify_dax_setup.sh

# Run benchmark
./scripts/run_tpcc.sh ./results

# Check logs for confirmation
grep "cxlalloc initialized" ./results/*.txt
# Should see: "backend=dax resource=/dev/dax0.0"
```

## Summary

**Golden Rule**: When using mmap or DAX backend, ALWAYS set both:
1. `CXL_BACKEND` = "mmap" or "dax"
2. `CXL_MEMORY_RESOURCE` = path to file or device (NOT "SS")

**For default behavior**: Don't set either variable, and tigon will use its original ivshmem mode.
