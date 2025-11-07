# Tigon Remote Host Configuration

## Overview

The tigon scripts have been updated to support both **localhost** (original) and **remote host** deployment modes.

## Changes Made

Modified `/workloads/tigon/scripts/utilities.sh` to support:
- Remote host deployment via `scp` to actual IP addresses
- Configurable IP ranges and SSH ports
- Backward compatibility with localhost mode

## Usage

### Method 1: Using Environment Variables (Recommended)

#### For Remote Hosts (192.168.0.10 - 192.168.0.17)

```bash
# Navigate to tigon directory
cd /root/CXLMemSim/workloads/tigon

# Set environment variables
export USE_REMOTE_HOSTS=1
export REMOTE_BASE_IP="192.168.0"
export REMOTE_START_SUFFIX=10
export REMOTE_PORT=22
export REMOTE_USER="root"

# Run setup for 8 hosts
./scripts/setup.sh VMS 8
```

#### For Localhost (Original Behavior)

```bash
# Use default or explicitly set
export USE_REMOTE_HOSTS=0

# Run setup
./scripts/setup.sh VMS 8
```

### Method 2: Using Configuration Script

```bash
# Navigate to tigon directory
cd /root/CXLMemSim/workloads/tigon

# Source the remote configuration
source ./scripts/remote_config.sh

# Edit the script to customize your IP range first if needed
# Then run your setup
./scripts/setup.sh VMS 8
```

## Configuration Options

| Variable | Default | Description |
|----------|---------|-------------|
| `USE_REMOTE_HOSTS` | `0` | Set to `1` for remote hosts, `0` for localhost |
| `REMOTE_BASE_IP` | `192.168.0` | Base IP address (without the last octet) |
| `REMOTE_START_SUFFIX` | `10` | Starting suffix for IP addresses |
| `REMOTE_PORT` | `22` | SSH port for remote hosts |
| `REMOTE_USER` | `root` | SSH username for remote hosts |

## IP Address Mapping

The VM ID maps to IP addresses as follows:

```
VM 0 -> ${REMOTE_BASE_IP}.${REMOTE_START_SUFFIX}
VM 1 -> ${REMOTE_BASE_IP}.${REMOTE_START_SUFFIX + 1}
VM 2 -> ${REMOTE_BASE_IP}.${REMOTE_START_SUFFIX + 2}
...
```

### Examples

#### Example 1: 192.168.0.10 - 192.168.0.17 (8 hosts)

```bash
export USE_REMOTE_HOSTS=1
export REMOTE_BASE_IP="192.168.0"
export REMOTE_START_SUFFIX=10
./scripts/setup.sh VMS 8
```

Hosts used:
- VM 0: 192.168.0.10
- VM 1: 192.168.0.11
- ...
- VM 7: 192.168.0.17

#### Example 2: 10.0.1.100 - 10.0.1.115 (16 hosts)

```bash
export USE_REMOTE_HOSTS=1
export REMOTE_BASE_IP="10.0.1"
export REMOTE_START_SUFFIX=100
./scripts/setup.sh VMS 16
```

Hosts used:
- VM 0: 10.0.1.100
- VM 1: 10.0.1.101
- ...
- VM 15: 10.0.1.115

#### Example 3: Custom SSH Port

```bash
export USE_REMOTE_HOSTS=1
export REMOTE_BASE_IP="192.168.1"
export REMOTE_START_SUFFIX=50
export REMOTE_PORT=2222
export REMOTE_USER="ubuntu"
./scripts/setup.sh VMS 4
```

Hosts used:
- VM 0: ubuntu@192.168.1.50:2222
- VM 1: ubuntu@192.168.1.51:2222
- VM 2: ubuntu@192.168.1.52:2222
- VM 3: ubuntu@192.168.1.53:2222

## What Gets Synchronized

When running `./scripts/setup.sh VMS <HOST_NUM>`, the following files are copied to each remote host:

1. **Kernel modules:**
   - `/dependencies/kernel_module/cxl_init` → `/root/cxl_init`
   - `/dependencies/kernel_module/cxl_recover_meta` → `/root/cxl_recover_meta`
   - `/dependencies/kernel_module/cxl_ivpci.ko` → `/root/cxl_ivpci.ko`

2. **Dependencies:**
   - `libjemalloc.so.2`
   - `libglog.so.0`
   - `libgflags.so.2.2`

3. **Commands executed on each host:**
   - Remove old kernel module: `rmmod cxl_ivpci`
   - Load new kernel module: `insmod ./cxl_ivpci.ko`

## SSH Key Setup

Ensure SSH keys are properly configured for passwordless access:

```bash
# Generate SSH key if not exists
[ -f ~/.ssh/id_rsa ] || ssh-keygen -t rsa -N "" -f ~/.ssh/id_rsa

# Copy to remote hosts (for each target host)
for i in {10..17}; do
    ssh-copy-id -p 22 root@192.168.0.$i
done
```

## Testing the Configuration

### Test SSH connectivity

```bash
# Set your configuration
export USE_REMOTE_HOSTS=1
export REMOTE_BASE_IP="192.168.0"
export REMOTE_START_SUFFIX=10

# Source utilities to use the functions
source ./scripts/utilities.sh

# Test SSH to VM 0 (192.168.0.10)
ssh_command "hostname" 0

# Test SSH to VM 1 (192.168.0.11)
ssh_command "hostname" 1
```

### Test file sync

```bash
# Create a test file
echo "test" > /tmp/test.txt

# Sync to 2 hosts
sync_files /tmp/test.txt /tmp/test.txt 2

# Verify
ssh_command "cat /tmp/test.txt" 0
ssh_command "cat /tmp/test.txt" 1
```

## Switching Between Modes

### Switch to Remote Mode

```bash
export USE_REMOTE_HOSTS=1
export REMOTE_BASE_IP="192.168.0"
export REMOTE_START_SUFFIX=10
```

### Switch to Localhost Mode

```bash
export USE_REMOTE_HOSTS=0
```

Or simply unset the variable:

```bash
unset USE_REMOTE_HOSTS
```

## Troubleshooting

### Connection Issues

1. **Check network connectivity:**
   ```bash
   ping 192.168.0.10
   ```

2. **Test SSH manually:**
   ```bash
   ssh -p 22 root@192.168.0.10
   ```

3. **Check SSH key:**
   ```bash
   ssh-copy-id -p 22 root@192.168.0.10
   ```

### Permission Issues

1. **Ensure remote user has permissions:**
   ```bash
   # Test write permissions
   ssh root@192.168.0.10 "touch /root/test.txt && rm /root/test.txt"
   ```

### Debugging

Enable verbose output:

```bash
# Edit utilities.sh and uncomment:
set -x

# Or run with bash -x
bash -x ./scripts/setup.sh VMS 8
```

## Original vs Remote Mode Comparison

| Feature | Localhost Mode | Remote Mode |
|---------|---------------|-------------|
| Target | 127.0.0.1 | Actual IP addresses |
| Port Range | 10022+ | Single port (default 22) |
| Use Case | Local QEMU VMs | Physical/remote machines |
| SSH Command | `ssh -p 1002X root@127.0.0.1` | `ssh -p 22 root@192.168.0.X` |
| SCP Command | `scp -P 1002X ... root@127.0.0.1:` | `scp -P 22 ... root@192.168.0.X:` |

## Integration with Other Scripts

All tigon scripts that use `ssh_command` or `sync_files` will automatically respect the remote host configuration:

- `setup.sh` - Initial setup and file sync
- `run.sh` - Running benchmarks
- `run_tpcc.sh`, `run_ycsb.sh`, etc. - Specific benchmark runs
- Any custom scripts using utilities.sh

## Example Workflow

```bash
# 1. Navigate to tigon
cd /root/CXLMemSim/workloads/tigon

# 2. Configure for 8 remote hosts (192.168.0.10 - 192.168.0.17)
export USE_REMOTE_HOSTS=1
export REMOTE_BASE_IP="192.168.0"
export REMOTE_START_SUFFIX=10

# 3. Setup remote hosts
./scripts/setup.sh VMS 8

# 4. Run benchmarks
./scripts/run_tpcc.sh ./results

# 5. Collect results (files will be on remote hosts)
# You can pull them back with reverse scp
for i in {0..7}; do
    ip=$((10 + i))
    scp -r root@192.168.0.$ip:/root/results ./results/host_$i/
done
```

## Notes

- The original localhost functionality is preserved and remains the default
- All changes are backward compatible
- SSH options include `-o StrictHostKeyChecking=no` for automated deployment
- File transfers use `-o LogLevel=ERROR` to reduce noise
- The `-r` flag enables recursive directory copying

## Support

For issues or questions:
1. Verify environment variables are set correctly
2. Test SSH connectivity manually
3. Check that target hosts have required dependencies
4. Ensure SSH keys are properly configured
