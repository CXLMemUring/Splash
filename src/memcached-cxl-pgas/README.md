# Memcached CXL Disaggregation with PGAS Abstraction

A bpftime-based runtime that intercepts memcached operations and disaggregates them across CXL-connected hosts using a PGAS (Partitioned Global Address Space) abstraction.

## Overview

This project provides transparent memory disaggregation for memcached by:

1. **Intercepting** memcached operations using bpftime uprobes
2. **Routing** key-value requests to appropriate CXL memory nodes
3. **Storing** data in CXL-attached memory across multiple hosts
4. **Communicating** between hosts using socket-based PGAS primitives

## Architecture

```
┌─────────────────┐     ┌─────────────────┐
│   Host 0        │     │   Host 1        │
│                 │     │                 │
│  ┌───────────┐  │     │  ┌───────────┐  │
│  │ memcached │  │     │  │ memcached │  │
│  └─────┬─────┘  │     │  └─────┬─────┘  │
│        │        │     │        │        │
│  ┌─────┴─────┐  │     │  ┌─────┴─────┐  │
│  │  bpftime  │  │     │  │  bpftime  │  │
│  │ uprobes   │  │     │  │ uprobes   │  │
│  └─────┬─────┘  │     │  └─────┬─────┘  │
│        │        │     │        │        │
│  ┌─────┴─────┐  │     │  ┌─────┴─────┐  │
│  │   PGAS    │◄─┼─────┼──►   PGAS    │  │
│  │  Runtime  │  │     │  │  Runtime  │  │
│  └─────┬─────┘  │     │  └─────┬─────┘  │
│        │        │     │        │        │
│  ┌─────┴─────┐  │     │  ┌─────┴─────┐  │
│  │CXL Memory │  │     │  │CXL Memory │  │
│  └───────────┘  │     │  └───────────┘  │
└────────┬────────┘     └────────┬────────┘
         │                       │
         └──────────┬────────────┘
                    │
            ┌───────┴───────┐
            │  CXL Switch   │
            └───────────────┘
```

## Components

### 1. PGAS Layer (`pgas.h/pgas.c`)

Provides a Partitioned Global Address Space abstraction:
- **Global pointers** for addressing memory across nodes
- **Remote memory access** (get/put operations)
- **Atomic operations** (fetch-add, CAS)
- **Synchronization** (barriers, fences)
- **Memory allocation** with affinity hints

### 2. CXL Memory Manager (`cxl_memory.h/cxl_memory.c`)

Manages CXL-attached memory:
- Device discovery (via sysfs/DAX)
- Memory region mapping
- Allocator with cache-line alignment
- Cache coherency operations (flush, invalidate)
- Memory barriers (mfence, sfence, lfence)

### 3. Memcached Interceptor (`memcached_interceptor.h/memcached_interceptor.c`)

Intercepts and redirects memcached operations:
- Key-based routing using consistent hashing
- Item storage/retrieval on CXL memory
- Local cache for frequently accessed items
- Statistics tracking (latency, hit rates)

### 4. BPF Programs (`bpf/memcached_uprobe.bpf.c`)

bpftime uprobes for memcached interception:
- `process_command` - Text protocol commands
- `item_get` - Item retrieval
- `do_item_alloc` - Item allocation
- `item_link/unlink` - Item linking/deletion

## Building

### Prerequisites

```bash
# Ubuntu/Debian
sudo apt-get install build-essential cmake clang llvm libbpf-dev

# RHEL/CentOS
sudo yum install gcc cmake clang llvm libbpf-devel
```

### Compile

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

### Install

```bash
sudo make install
```

## Configuration

Create a configuration file based on the example:

```bash
cp config/nodes.conf.example /etc/memcached-cxl-pgas/nodes.conf
```

Edit with your node information:

```ini
local_node_id=0
num_nodes=2
node0=192.168.1.10:5000:0x100000000:1073741824
node1=192.168.1.11:5000:0x100000000:1073741824
```

### Finding CXL Memory Addresses

```bash
# For CXL devices
cat /proc/iomem | grep -i cxl

# For DAX devices
ls -la /dev/dax*
cat /sys/class/dax/dax*/resource
cat /sys/class/dax/dax*/size
```

## Usage

### Start on Each Host

```bash
# Host 0
./memcached_cxl_pgas -c /etc/memcached-cxl-pgas/nodes.conf \
    -m /usr/bin/memcached -s 128

# Host 1
./memcached_cxl_pgas -c /etc/memcached-cxl-pgas/nodes.conf \
    -m /usr/bin/memcached -s 128
```

### Command Line Options

```
-c, --config FILE       Configuration file (required)
-m, --memcached PATH    Memcached binary path
-p, --pid PID           Attach to running memcached
-s, --cache-size SIZE   Local cache size in MB (default: 64)
-t, --hash-table SIZE   Hash table size (default: 1M)
-r, --replicate N       Enable replication with factor N
--no-cxl                Disable CXL (local only)
--stats-interval SEC    Stats interval (default: 10)
-h, --help              Show help
```

### Example Configurations

**Basic 2-node setup:**
```bash
./memcached_cxl_pgas -c nodes.conf -m /usr/bin/memcached
```

**With 256MB local cache and replication:**
```bash
./memcached_cxl_pgas -c nodes.conf -m /usr/bin/memcached -s 256 -r 2
```

**High-performance with large hash table:**
```bash
./memcached_cxl_pgas -c nodes.conf -m /usr/bin/memcached -s 512 -t 10000000
```

## How It Works

### Request Flow

1. **Interception**: bpftime uprobes capture memcached function calls
2. **Key Hashing**: MurmurHash3-like hash computes key hash
3. **Routing**: Hash determines target node (`hash % num_nodes`)
4. **Local Check**: If target is local node, use local CXL memory
5. **Remote Access**: Otherwise, use PGAS get/put to remote node
6. **Response**: Return data to memcached

### Memory Layout

Each item stored in CXL memory consists of:
- **Metadata** (mc_item_meta_t): hash, sizes, flags, expiration, PGAS pointer
- **Data**: key bytes followed by value bytes

### Consistency Model

The system uses release consistency by default:
- Writers use **release** semantics (data visible after flag)
- Readers use **acquire** semantics (see flag, then data)
- Explicit fences for stronger guarantees

## Performance Considerations

### Tuning Parameters

1. **Local Cache Size**: Larger cache reduces remote accesses
2. **Hash Table Size**: More buckets = fewer collisions
3. **Batch Size**: Group multiple operations for efficiency
4. **Prefetch Depth**: Speculative fetches for sequential access

### Optimization Tips

1. Use NUMA-aware allocation on each host
2. Enable huge pages for CXL memory regions
3. Pin runtime threads to specific cores
4. Use RDMA if available for lower latency

### Expected Latencies

- Local CXL access: ~200-500 ns
- Remote CXL access: ~1-5 μs (depending on switch)
- Network overhead: ~10-50 μs per message

## Monitoring

The runtime prints periodic statistics:

```
=== Memcached Interceptor Statistics ===
Total requests: 1000000
Local hits: 498000 (49.80%)
Remote hits: 502000 (50.20%)
Cache hits: 850000, misses: 150000 (85.00% hit rate)
CXL reads: 54400000 bytes, writes: 32000000 bytes
Avg latency: 2.34 μs, P99: 12.56 μs
========================================
```

## Limitations

1. **Text Protocol Only**: Binary protocol support is partial
2. **Key Size**: Limited by BPF constraints (~250 bytes)
3. **Value Size**: Large values may need chunking
4. **Consistency**: Not linearizable for concurrent updates to same key

## Troubleshooting

### Common Issues

**"Failed to initialize PGAS"**
- Check configuration file path and format
- Verify all nodes are reachable

**"Could not allocate remote hash table"**
- Insufficient CXL memory
- Check cxl_size in configuration

**"Failed to attach uprobes"**
- Ensure memcached is running
- Check memcached binary has debug symbols

### Debug Mode

```bash
# Enable verbose logging
export PGAS_DEBUG=1
./memcached_cxl_pgas -c nodes.conf -m /usr/bin/memcached
```

## Future Work

- [ ] Binary protocol support
- [ ] RDMA transport layer
- [ ] Multi-get batching
- [ ] Consistent hashing with virtual nodes
- [ ] Hot key detection and migration
- [ ] Persistence to CXL-attached NVM

## References

- [CXL Specification](https://www.computeexpresslink.org/)
- [bpftime](https://github.com/eunomia-bpf/bpftime)
- [memcached](https://memcached.org/)
- [PGAS Languages](https://en.wikipedia.org/wiki/Partitioned_global_address_space)

## License

Research and experimental use.
