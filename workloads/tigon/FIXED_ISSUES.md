# Fixed Issues and How to Use Tigon

## Issues Fixed

### ✅ Issue 1: Command Breaking Across Lines
**Symptom:**
```
bash: line 2: --threads=3: command not found
bash: line 3: --log_path=/root/pasha_log: No such file or directory
```

**Cause:** `$CXL_BACKEND_FLAGS` was on separate lines in the SSH command

**Fix:** Moved `$CXL_BACKEND_FLAGS` to the same line as other flags

### ✅ Issue 2: CXL Backend Error
**Symptom:**
```
Check failed: resource != "SS" Backend mmap requires --cxl_memory_resource to point at a file or device
```

**Cause:** `CXL_BACKEND` was set but `CXL_MEMORY_RESOURCE` was still "SS"

**Fix:** Added smart logic to only set flags when both are properly configured

### ✅ Issue 3: Server IP Mismatch
**Symptom:** Server string used 192.168.100.2-9 but SSH used 192.168.100.10-17

**Fix:** Updated `print_server_string()` to use `REMOTE_START_SUFFIX` (default: 10)

## How to Use Tigon (Three Modes)

### Mode 1: Default (IVSHMEM) - Simplest

**When to use:** Testing on QEMU VMs with original IVSHMEM setup

**Setup:**
```bash
# Clear any previous configuration
source ./scripts/reset_config.sh

# Run directly
cd /root/CXLMemSim/workloads/tigon
./scripts/run_tpcc.sh ./results
```

**What happens:**
- Uses IVSHMEM shared memory for CXL
- Connects to remote hosts at 192.168.100.10-17:22
- Server string: 192.168.100.10-17:1234

### Mode 2: DAX/mmap with File - For Testing

**When to use:** Testing DAX mode without actual DAX hardware

**Setup:**
```bash
# Clear previous config
source ./scripts/reset_config.sh

# Configure mmap with file
export CXL_BACKEND="mmap"
export CXL_MEMORY_RESOURCE="/tmp/cxl_memory"

# Create the file (optional - tigon will create it)
touch /tmp/cxl_memory

# Verify
echo "Backend: $CXL_BACKEND"
echo "Resource: $CXL_MEMORY_RESOURCE"

# Run
./scripts/run_tpcc.sh ./results
```

**What happens:**
- Uses mmap on /tmp/cxl_memory
- Connects to remote hosts at 192.168.100.10-17:22
- Tigon will create and manage the file

### Mode 3: DAX with Real Device - Production

**When to use:** Production deployment with actual DAX/CXL hardware

**Setup:**
```bash
# Clear previous config
source ./scripts/reset_config.sh

# Inside QEMU guest, create DAX namespace first:
# cxl create-region -d decoder0.0 -w 1 mem0
# ndctl create-namespace -r region0 -m devdax

# Verify device exists
ls -l /dev/dax0.0

# Configure DAX mode
export CXL_BACKEND="dax"
export CXL_MEMORY_RESOURCE="/dev/dax0.0"

# Verify
echo "Backend: $CXL_BACKEND"
echo "Resource: $CXL_MEMORY_RESOURCE"
ls -lh $CXL_MEMORY_RESOURCE

# Run
./scripts/run_tpcc.sh ./results
```

**What happens:**
- Uses direct DAX device access
- Connects to remote hosts at 192.168.100.10-17:22
- Best performance

## Quick Command Reference

### Check Current Configuration
```bash
echo "CXL_BACKEND: ${CXL_BACKEND:-(not set)}"
echo "CXL_MEMORY_RESOURCE: ${CXL_MEMORY_RESOURCE:-(not set)}"
echo "USE_REMOTE_HOSTS: ${USE_REMOTE_HOSTS:-(not set)}"
echo "REMOTE_BASE_IP: ${REMOTE_BASE_IP:-(not set)}"
echo "REMOTE_START_SUFFIX: ${REMOTE_START_SUFFIX:-(not set)}"
```

### Reset Configuration
```bash
source ./scripts/reset_config.sh
```

### Test Configuration
```bash
./scripts/verify_dax_setup.sh
./scripts/test_remote_connection.sh
```

### Run Benchmarks
```bash
# TPC-C
./scripts/run_tpcc.sh ./results

# YCSB
./scripts/run_ycsb.sh ./results
```

## Typical Workflow

### For QEMU VM Setup with DAX

```bash
#!/bin/bash
# complete_workflow.sh

cd /root/CXLMemSim/workloads/tigon

# Step 1: Reset any previous configuration
source ./scripts/reset_config.sh

# Step 2: Setup VMs (if needed)
# This syncs files to 192.168.100.10-17
./scripts/setup.sh VMS 8

# Step 3: Configure for DAX mode
export CXL_BACKEND="dax"
export CXL_MEMORY_RESOURCE="/dev/dax0.0"

# Step 4: Verify setup
./scripts/verify_dax_setup.sh

# Step 5: Run benchmark
./scripts/run_tpcc.sh ./results

# Step 6: Check logs
grep "cxlalloc initialized" ./results/*.txt
# Should see: "backend=dax resource=/dev/dax0.0"
```

### For Quick Testing Without DAX

```bash
#!/bin/bash
# quick_test.sh

cd /root/CXLMemSim/workloads/tigon

# Use file-based mmap
export CXL_BACKEND="mmap"
export CXL_MEMORY_RESOURCE="/tmp/cxl_test"

# Run
./scripts/run_ycsb.sh ./results
```

## Avoiding the Error

The error `Backend mmap requires --cxl_memory_resource to point at a file or device` happens when:

❌ **Wrong:**
```bash
export CXL_BACKEND="dax"
# Forgot to set CXL_MEMORY_RESOURCE
./scripts/run_tpcc.sh ./results
```

✅ **Correct:**
```bash
export CXL_BACKEND="dax"
export CXL_MEMORY_RESOURCE="/dev/dax0.0"
./scripts/run_tpcc.sh ./results
```

✅ **Or don't set anything:**
```bash
unset CXL_BACKEND
unset CXL_MEMORY_RESOURCE
./scripts/run_tpcc.sh ./results  # Uses IVSHMEM default
```

## Verification Checklist

Before running benchmarks:

```bash
# 1. Check environment
env | grep CXL

# 2. Verify configuration won't cause error
if [ "$CXL_BACKEND" = "dax" ] || [ "$CXL_BACKEND" = "mmap" ]; then
    if [ -z "$CXL_MEMORY_RESOURCE" ] || [ "$CXL_MEMORY_RESOURCE" = "SS" ]; then
        echo "ERROR: CXL_BACKEND=$CXL_BACKEND but CXL_MEMORY_RESOURCE not properly set!"
        echo "Fix: export CXL_MEMORY_RESOURCE=/dev/dax0.0"
    else
        echo "OK: Configuration valid"
    fi
else
    echo "OK: Using default mode"
fi

# 3. Verify remote hosts are reachable
./scripts/test_remote_connection.sh

# 4. Run verification
./scripts/verify_dax_setup.sh
```

## Log Verification

After running a benchmark, verify it used the correct backend:

```bash
# Check logs
grep -i "cxlalloc initialized" ./results/*.txt

# Expected outputs:
# Default mode:
#   "cxlalloc initialized for thread 0: backend=ivshmem resource=SS"

# DAX mode:
#   "cxlalloc initialized for thread 0: backend=dax resource=/dev/dax0.0"

# mmap mode:
#   "cxlalloc initialized for thread 0: backend=mmap resource=/tmp/cxl_memory"
```

## Environment Variable Priority

Tigon checks configuration in this order:

1. **Environment variables** (highest priority)
   ```bash
   export CXL_BACKEND="dax"
   export CXL_MEMORY_RESOURCE="/dev/dax0.0"
   ```

2. **Config scripts**
   ```bash
   source ./scripts/dax_config.sh
   ```

3. **Tigon defaults** (lowest priority)
   - Uses IVSHMEM mode if nothing is configured

## Troubleshooting

### Still Getting the Error?

```bash
# Check if variables are set in your shell
env | grep CXL

# Clear them
source ./scripts/reset_config.sh

# Try again
./scripts/run_tpcc.sh ./results
```

### DAX Device Not Found?

```bash
# Check what devices exist
ls -l /dev/dax*
ls -l /dev/pmem*

# In QEMU guest, create namespace
cxl create-region -d decoder0.0 -w 1 mem0
ndctl create-namespace -r region0 -m devdax

# Verify
ls -l /dev/dax0.0

# Then configure
export CXL_BACKEND="dax"
export CXL_MEMORY_RESOURCE="/dev/dax0.0"
```

### SSH Connection Fails?

```bash
# Test connection
ssh -p 22 root@192.168.100.10 hostname

# If fails, check network
ping 192.168.100.10

# Setup SSH keys
ssh-copy-id -p 22 root@192.168.100.10
```

## Summary

**Three Simple Rules:**

1. **For Default Mode**: Don't set CXL_BACKEND or CXL_MEMORY_RESOURCE
2. **For DAX/mmap Mode**: Set BOTH CXL_BACKEND and CXL_MEMORY_RESOURCE together
3. **When In Doubt**: Run `source ./scripts/reset_config.sh` and start fresh

**Golden Commands:**

```bash
# Reset and use defaults
source ./scripts/reset_config.sh
./scripts/run_tpcc.sh ./results

# Or configure DAX
source ./scripts/reset_config.sh
export CXL_BACKEND="dax"
export CXL_MEMORY_RESOURCE="/dev/dax0.0"
./scripts/run_tpcc.sh ./results
```

That's it! All issues are fixed and you have three clear modes to choose from.
