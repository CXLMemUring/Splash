# Tigon DAX/mmap Setup Guide

## Overview

This guide explains how to configure Tigon to use **mmap over DAX** (Direct Access) for CXL memory access on QEMU. This provides direct memory-mapped access to CXL memory without buffering through the page cache.

## Changes Made

1. **Modified `scripts/run.sh`**:
   - Added CXL backend configuration variables
   - Added `$CXL_BACKEND_FLAGS` to all benchmark command executions
   - Supports environment variable overrides

2. **Created `scripts/dax_config.sh`**:
   - Easy configuration script for DAX/mmap mode
   - Validates settings and checks for DAX devices
   - Source this before running benchmarks

3. **Updated remote host support** in `scripts/utilities.sh`:
   - Can now deploy to actual IP addresses (not just localhost)
   - See `REMOTE_SETUP.md` for remote deployment

## Backend Modes

Tigon supports three backend modes for CXL memory access:

| Backend | Description | Use Case |
|---------|-------------|----------|
| **ivshmem** (default) | Shared memory via IVSHMEM | Original QEMU-based emulation |
| **mmap** | Memory-mapped file/device | File-based or DAX device |
| **dax** | DAX device (uses mmap internally) | Best performance with actual DAX hardware |

## Quick Start

### 1. Using DAX Device in QEMU

```bash
# Configure for DAX mode
export CXL_BACKEND="dax"
export CXL_MEMORY_RESOURCE="/dev/dax0.0"

# Or use the config script
source ./scripts/dax_config.sh

# Run benchmarks
./scripts/run_tpcc.sh ./results
```

### 2. Using File-based mmap (Testing)

```bash
# Configure for file-based mmap
export CXL_BACKEND="mmap"
export CXL_MEMORY_RESOURCE="/tmp/cxl_memory"

# Run benchmarks
./scripts/run_ycsb.sh ./results
```

### 3. Using Original IVSHMEM Mode

```bash
# Keep defaults (or explicitly set)
export CXL_BACKEND="mmap"  # or "ivshmem"
export CXL_MEMORY_RESOURCE="SS"

# Run benchmarks
./scripts/run_tpcc.sh ./results
```

## QEMU Configuration for DAX

### Option 1: Using CXL Type 3 with DAX-backed Memory

Modify your QEMU launch script to use DAX-backed memory:

```bash
#!/bin/bash

QEMU=/usr/local/bin/qemu-system-x86_64

# Create DAX device in QEMU
$QEMU \
    --enable-kvm \
    -M q35,cxl=on \
    -m 16G,maxmem=128G,slots=8 \
    -smp 4 \
    \
    # CXL Root Port
    -device pxb-cxl,bus_nr=12,bus=pcie.0,id=cxl.1 \
    -device cxl-rp,port=0,bus=cxl.1,id=root_port13,chassis=0,slot=0 \
    \
    # CXL Type 3 device with DAX-backed memory
    -object memory-backend-file,id=cxl-mem1,share=on,mem-path=/dev/dax0.0,size=64G,align=2M \
    -device cxl-type3,bus=root_port13,persistent-memdev=cxl-mem1,id=cxl-pmem0,sn=0x1 \
    \
    # CXL Fixed Memory Window
    -M cxl-fmw.0.targets.0=cxl.1,cxl-fmw.0.size=64G \
    \
    # Other options...
    -nographic
```

### Option 2: Using File-backed Memory (for testing)

```bash
# Create backing file
dd if=/dev/zero of=/tmp/cxl_backing bs=1G count=64

# In QEMU
-object memory-backend-file,id=cxl-mem1,share=on,mem-path=/tmp/cxl_backing,size=64G \
-device cxl-type3,bus=root_port13,persistent-memdev=cxl-mem1,id=cxl-pmem0,sn=0x1
```

### Option 3: Using PMEM Device

If you have actual persistent memory:

```bash
# In QEMU
-object memory-backend-file,id=cxl-mem1,share=on,mem-path=/dev/pmem0,size=64G \
-device cxl-type3,bus=root_port13,persistent-memdev=cxl-mem1,id=cxl-pmem0,sn=0x1
```

## Inside QEMU Guest Setup

After booting the QEMU guest with CXL devices:

### 1. Check CXL Devices

```bash
# List CXL devices
ls -l /sys/bus/cxl/devices/

# Check memory regions
lsmem
```

### 2. Initialize CXL Memory

```bash
# Create DAX device (if using devdax mode)
ndctl create-namespace -m devdax -e namespace0.0

# Or use fsdax mode and mount
ndctl create-namespace -m fsdax -e namespace0.0
mkdir -p /mnt/pmem0
mount -o dax /dev/pmem0 /mnt/pmem0
```

### 3. Configure Tigon to Use DAX

```bash
# For devdax mode
export CXL_BACKEND="dax"
export CXL_MEMORY_RESOURCE="/dev/dax0.0"

# For fsdax mode
export CXL_BACKEND="mmap"
export CXL_MEMORY_RESOURCE="/mnt/pmem0/cxl_memory"

# Verify
echo "Backend: $CXL_BACKEND"
echo "Resource: $CXL_MEMORY_RESOURCE"
ls -lh $CXL_MEMORY_RESOURCE
```

## Configuration Hierarchy

Tigon checks for CXL backend configuration in this order:

1. **Environment Variables** (highest priority):
   ```bash
   export CXL_BACKEND="dax"
   export CXL_MEMORY_RESOURCE="/dev/dax0.0"
   ```

2. **dax_config.sh script**:
   ```bash
   source ./scripts/dax_config.sh
   ```

3. **Defaults in run.sh**:
   - `CXL_BACKEND="mmap"`
   - `CXL_MEMORY_RESOURCE="SS"` (shared memory/ivshmem)

## DAX vs Regular File Performance

| Aspect | DAX Mode | Regular File | IVSHMEM |
|--------|----------|--------------|---------|
| **Page Cache** | Bypassed | Used | N/A |
| **Latency** | Lowest | Higher | Medium |
| **CPU Overhead** | Minimal | Higher (syscalls) | Medium |
| **Memory Copy** | Direct | Required | Shared |
| **Best For** | Production | Testing | Emulation |

## Verifying DAX is Active

### Method 1: Check Process Maps

```bash
# Run tigon benchmark
./bench_tpcc --logtostderr=1 --id=0 ... &

# Check memory maps
PID=$(pgrep bench_tpcc)
cat /proc/$PID/maps | grep dax
```

You should see mappings to your DAX device.

### Method 2: Monitor with `daxctl`

```bash
# Install daxctl
apt-get install daxctl

# Monitor DAX device
daxctl list
watch -n 1 'daxctl list'
```

### Method 3: Check tigon Logs

```bash
# Tigon will log the backend initialization
./bench_tpcc --logtostderr=1 ... 2>&1 | grep cxlalloc

# Look for:
# "cxlalloc initialized for thread 0: backend=dax resource=/dev/dax0.0"
```

## Complete Example Workflow

### Step 1: Prepare Host

```bash
# Install dependencies
sudo apt-get install ndctl daxctl

# Check for DAX devices
ls -l /dev/dax*
```

### Step 2: Configure QEMU with DAX

Create `launch_qemu_dax.sh`:

```bash
#!/bin/bash

QEMU=/usr/local/bin/qemu-system-x86_64

# Ensure DAX device exists on host
if [ ! -e /dev/dax0.0 ]; then
    echo "DAX device /dev/dax0.0 not found!"
    echo "Create it with: ndctl create-namespace -m devdax"
    exit 1
fi

exec $QEMU \
    --enable-kvm \
    -M q35,cxl=on \
    -m 16G,maxmem=128G,slots=8 \
    -smp 4 \
    -device pxb-cxl,bus_nr=12,bus=pcie.0,id=cxl.1 \
    -device cxl-rp,port=0,bus=cxl.1,id=root_port13,chassis=0,slot=0 \
    -object memory-backend-file,id=cxl-mem1,share=on,mem-path=/dev/dax0.0,size=64G,align=2M \
    -device cxl-type3,bus=root_port13,persistent-memdev=cxl-mem1,id=cxl-pmem0,sn=0x1 \
    -M cxl-fmw.0.targets.0=cxl.1,cxl-fmw.0.size=64G \
    -kernel ./bzImage \
    -append "root=/dev/sda rw console=ttyS0,115200" \
    -drive file=./qemu.img,format=qcow2 \
    -nographic
```

### Step 3: Setup Inside Guest

```bash
# After QEMU boots
ssh -p 10022 root@localhost

# Inside guest - check CXL device
ls /sys/bus/cxl/devices/
# Should see: mem0

# Create namespace
cxl create-region -d decoder0.0 -w 1 mem0
ndctl create-namespace -r region0 -m devdax

# Verify DAX device
ls -l /dev/dax0.0
```

### Step 4: Configure and Run Tigon

```bash
# Still inside guest
cd /root/pasha  # or wherever tigon is

# Configure DAX mode
export CXL_BACKEND="dax"
export CXL_MEMORY_RESOURCE="/dev/dax0.0"

# Verify configuration
echo "Backend: $CXL_BACKEND"
echo "Resource: $CXL_MEMORY_RESOURCE"

# Run benchmark
./bench_tpcc --logtostderr=1 --id=0 --servers="localhost:18000" \
    --threads=4 --partition_num=4 \
    --cxl_backend=$CXL_BACKEND \
    --cxl_memory_resource=$CXL_MEMORY_RESOURCE \
    --time_to_run=30 --time_to_warmup=10 \
    --protocol=SundialPasha

# Check logs for confirmation
# Should see: "cxlalloc initialized for thread 0: backend=dax resource=/dev/dax0.0"
```

## Troubleshooting

### Issue: DAX device not found

```bash
# Create DAX namespace
ndctl create-namespace -m devdax -e namespace0.0

# Or use fsdax and mount
ndctl create-namespace -m fsdax -e namespace0.0
mkdir -p /mnt/pmem0
mount -o dax /dev/pmem0 /mnt/pmem0
```

### Issue: Permission denied accessing /dev/dax0.0

```bash
# Check permissions
ls -l /dev/dax0.0

# Fix permissions
sudo chmod 666 /dev/dax0.0

# Or run as root
sudo -E ./bench_tpcc ...
```

### Issue: Backend falls back to shared memory

Check tigon logs:

```bash
# If you see:
# "Backend dax requires --cxl_memory_resource to point at a file or device"

# Make sure CXL_MEMORY_RESOURCE is not "SS"
echo $CXL_MEMORY_RESOURCE  # Should NOT be "SS"
```

### Issue: mmap fails with EINVAL

```bash
# Check DAX device size
cat /sys/bus/nd/devices/namespace0.0/size

# Ensure alignment (usually 2MB for DAX)
# Recreate with proper alignment:
ndctl create-namespace -m devdax -a 2M
```

## Performance Comparison

Expected performance improvements with DAX:

| Metric | IVSHMEM | mmap (file) | mmap (DAX) |
|--------|---------|-------------|------------|
| **Latency** | ~500ns | ~800ns | ~200ns |
| **Throughput** | ~10 GB/s | ~8 GB/s | ~15 GB/s |
| **CPU %** | 30% | 45% | 20% |

*Actual numbers depend on hardware and workload*

## References

- [NVDIMM and DAX Documentation](https://www.kernel.org/doc/html/latest/driver-api/nvdimm/nvdimm.html)
- [QEMU CXL Support](https://qemu.readthedocs.io/en/latest/system/devices/cxl.html)
- [CXL Specification](https://www.computeexpresslink.org/spec)
- [ndctl/daxctl Tools](https://github.com/pmem/ndctl)

## Summary

To use mmap over DAX in Tigon:

1. **Configure QEMU** with DAX-backed CXL memory
2. **Inside guest**: Create DAX namespace with `ndctl`
3. **Configure Tigon**: Set `CXL_BACKEND=dax` and `CXL_MEMORY_RESOURCE=/dev/dax0.0`
4. **Run benchmarks**: Scripts will automatically use DAX mode

For testing without actual DAX hardware, use file-based mmap with `CXL_MEMORY_RESOURCE=/tmp/cxl_memory`.
