# Tigon Configuration Changes Summary

## Changes Made

### 1. Remote Host Support (utilities.sh)
**File**: `scripts/utilities.sh`

**Changes**:
- Added support for remote IP addresses instead of just localhost
- Configuration via environment variables:
  - `USE_REMOTE_HOSTS=1` - Enable remote mode
  - `REMOTE_BASE_IP="192.168.100"` - Base IP address
  - `REMOTE_START_SUFFIX=10` - Starting suffix (VM 0 = .10, VM 1 = .11, etc.)
  - `REMOTE_PORT=22` - SSH port
  - `REMOTE_USER=root` - SSH username

**Mapping**:
```
VM 0 -> 192.168.100.10:22
VM 1 -> 192.168.100.11:22
VM 2 -> 192.168.100.12:22
...
VM 7 -> 192.168.100.17:22
```

**Backward Compatibility**: Original localhost mode is preserved when `USE_REMOTE_HOSTS=0` (default)

### 2. DAX/mmap Support (run.sh)
**File**: `scripts/run.sh`

**Changes**:
- Added CXL backend configuration
- Passes `--cxl_backend` and `--cxl_memory_resource` to all benchmarks
- Smart default handling to avoid configuration conflicts
- Configuration via environment variables:
  - `CXL_BACKEND="dax"` or `"mmap"` - Backend mode
  - `CXL_MEMORY_RESOURCE="/dev/dax0.0"` - DAX device or file path

**Default Behavior**: Uses tigon's internal defaults (ivshmem mode) when not configured

### 3. Server String Generation Fix (run.sh)
**File**: `scripts/run.sh` - `print_server_string()` function

**Changes**:
- Changed from hardcoded `base=2` to use `REMOTE_START_SUFFIX` (default: 10)
- Now respects `REMOTE_BASE_IP` environment variable
- Generates server strings matching SSH target IPs

**Before**:
```
--servers="192.168.100.2:1234;192.168.100.3:1234;...;192.168.100.9:1234"
```

**After**:
```
--servers="192.168.100.10:1234;192.168.100.11:1234;...;192.168.100.17:1234"
```

## Configuration Files Created

### 1. dax_config.sh
**Purpose**: Easy configuration for DAX/mmap mode
**Usage**:
```bash
source ./scripts/dax_config.sh
./scripts/run_tpcc.sh ./results
```

### 2. remote_config.sh
**Purpose**: Easy configuration for remote host deployment
**Usage**:
```bash
source ./scripts/remote_config.sh
./scripts/setup.sh VMS 8
```

### 3. verify_dax_setup.sh
**Purpose**: Verify DAX/mmap configuration
**Usage**:
```bash
./scripts/verify_dax_setup.sh
```

### 4. test_remote_connection.sh
**Purpose**: Test remote host connectivity
**Usage**:
```bash
./scripts/test_remote_connection.sh
```

### 5. QUICKSTART_REMOTE.sh
**Purpose**: Interactive setup for remote deployment
**Usage**:
```bash
./scripts/QUICKSTART_REMOTE.sh
```

## Documentation Created

1. **DAX_MMAP_SETUP.md** - Complete guide for DAX/mmap configuration
2. **QUICK_DAX_GUIDE.md** - Quick reference for DAX setup
3. **REMOTE_SETUP.md** - Remote host deployment guide
4. **CHANGES_SUMMARY.md** - This file

## Usage Examples

### Example 1: Remote Deployment with DAX

```bash
# Configure remote hosts
export USE_REMOTE_HOSTS=1
export REMOTE_BASE_IP="192.168.100"
export REMOTE_START_SUFFIX=10

# Configure DAX mode
export CXL_BACKEND="dax"
export CXL_MEMORY_RESOURCE="/dev/dax0.0"

# Setup and run
cd /root/CXLMemSim/workloads/tigon
./scripts/setup.sh VMS 8
./scripts/run_tpcc.sh ./results
```

### Example 2: Local Testing with mmap

```bash
# No remote hosts (use localhost)
unset USE_REMOTE_HOSTS

# Configure mmap with file
export CXL_BACKEND="mmap"
export CXL_MEMORY_RESOURCE="/tmp/cxl_memory"

# Run
./scripts/run_ycsb.sh ./results
```

### Example 3: Default IVSHMEM Mode

```bash
# No special configuration needed
unset CXL_BACKEND
unset CXL_MEMORY_RESOURCE
unset USE_REMOTE_HOSTS

# Run with original behavior
./scripts/run_tpcc.sh ./results
```

## Compatibility Matrix

| Configuration | SSH Targets | Server String | CXL Backend | Result |
|---------------|-------------|---------------|-------------|--------|
| Default (no env vars) | localhost:1002X | 192.168.100.10-17:1234 | ivshmem | ✅ Works |
| `USE_REMOTE_HOSTS=1` | 192.168.100.10-17:22 | 192.168.100.10-17:1234 | ivshmem | ✅ Works |
| `CXL_BACKEND=dax` + `CXL_MEMORY_RESOURCE=/dev/dax0.0` | (any) | (any) | DAX | ✅ Works |
| `CXL_BACKEND=mmap` + `CXL_MEMORY_RESOURCE=SS` | (any) | (any) | ERROR | ❌ Fails |

## Key Points

1. **Consistent IP Mapping**: SSH connections and server strings now use the same IP range (192.168.100.10-17)

2. **DAX Requires Both Variables**: Must set both `CXL_BACKEND` and `CXL_MEMORY_RESOURCE` together

3. **Backward Compatible**: Original behavior preserved when environment variables are not set

4. **Flexible Deployment**: Supports localhost, remote hosts, and mixed scenarios

## Testing

### Test Remote Configuration
```bash
source ./scripts/utilities.sh
ssh_command "hostname" 0  # Should connect to 192.168.100.10
ssh_command "hostname" 1  # Should connect to 192.168.100.11
```

### Test Server String
```bash
source ./scripts/run.sh
print_server_string 8
# Should output: 192.168.100.10:1234;192.168.100.11:1234;...;192.168.100.17:1234
```

### Test DAX Configuration
```bash
export CXL_BACKEND="dax"
export CXL_MEMORY_RESOURCE="/dev/dax0.0"
./scripts/verify_dax_setup.sh
```

## Troubleshooting

### Issue: Server string mismatch
**Symptom**: Benchmarks can't connect to each other
**Solution**: Ensure `REMOTE_BASE_IP` and `REMOTE_START_SUFFIX` are consistent

### Issue: DAX backend error
**Symptom**: "Backend mmap requires --cxl_memory_resource to point at a file or device"
**Solution**: Set both `CXL_BACKEND` and `CXL_MEMORY_RESOURCE`:
```bash
export CXL_BACKEND="dax"
export CXL_MEMORY_RESOURCE="/dev/dax0.0"
```

### Issue: SSH connection fails
**Symptom**: Can't connect to remote hosts
**Solution**: Verify hosts are accessible and SSH keys are set up:
```bash
ssh-copy-id -p 22 root@192.168.100.10
```

## Migration from Original Setup

### If using localhost (original):
**No changes needed** - Everything works as before

### If using VMs with ports 10022+:
**Update to**:
```bash
# Use localhost mode (keeps port-based SSH)
export USE_REMOTE_HOSTS=0
# Or explicitly set it in utilities.sh
```

### If using actual remote hosts:
**Update to**:
```bash
export USE_REMOTE_HOSTS=1
export REMOTE_BASE_IP="192.168.100"  # Your subnet
export REMOTE_START_SUFFIX=10        # Starting suffix
```

## Files Modified

- ✅ `scripts/utilities.sh` - Added remote host support
- ✅ `scripts/run.sh` - Added DAX/mmap support and fixed server string
- ✅ `scripts/dax_config.sh` - Created
- ✅ `scripts/remote_config.sh` - Created
- ✅ `scripts/verify_dax_setup.sh` - Created
- ✅ `scripts/test_remote_connection.sh` - Created
- ✅ `scripts/QUICKSTART_REMOTE.sh` - Created
- ✅ `DAX_MMAP_SETUP.md` - Created
- ✅ `QUICK_DAX_GUIDE.md` - Created
- ✅ `REMOTE_SETUP.md` - Created
- ✅ `CHANGES_SUMMARY.md` - Created

## Environment Variables Reference

### Remote Host Configuration
| Variable | Default | Description |
|----------|---------|-------------|
| `USE_REMOTE_HOSTS` | 0 | Enable remote host mode (1) or localhost (0) |
| `REMOTE_BASE_IP` | 192.168.100 | Base IP address |
| `REMOTE_START_SUFFIX` | 10 | Starting IP suffix |
| `REMOTE_PORT` | 22 | SSH port |
| `REMOTE_USER` | root | SSH username |

### DAX/mmap Configuration
| Variable | Default | Description |
|----------|---------|-------------|
| `CXL_BACKEND` | (not set) | Backend mode: "dax", "mmap", or "ivshmem" |
| `CXL_MEMORY_RESOURCE` | (not set) | Device/file path or "SS" for shared memory |

## Quick Reference

```bash
# Remote deployment with DAX
export USE_REMOTE_HOSTS=1
export REMOTE_BASE_IP="192.168.100"
export REMOTE_START_SUFFIX=10
export CXL_BACKEND="dax"
export CXL_MEMORY_RESOURCE="/dev/dax0.0"

# Run
./scripts/setup.sh VMS 8
./scripts/run_tpcc.sh ./results
```

That's it! The tigon scripts now support both remote host deployment and DAX/mmap memory access modes while maintaining full backward compatibility.
