# GAPBS with CXL PGAS

Distributed graph analytics using the GAP Benchmark Suite with CXL memory disaggregation via PGAS abstraction.

## Overview

This project extends GAPBS (Graph Algorithm Platform Benchmark Suite) to run on distributed CXL-connected systems using a PGAS (Partitioned Global Address Space) programming model. Graph data is partitioned across multiple hosts' CXL memory, enabling processing of graphs larger than single-machine capacity.

## Features

- **Distributed Graph Storage**: CSR format partitioned across CXL nodes
- **Transparent Remote Access**: PGAS abstraction hides distribution
- **Multiple Algorithms**: BFS, PageRank, Connected Components, Triangle Counting
- **OpenMP Parallelization**: Multi-threaded within each node
- **Flexible Partitioning**: Block, cyclic, or custom schemes

## Architecture

```
┌────────────────────┐     ┌────────────────────┐
│      Host 0        │     │      Host 1        │
│                    │     │                    │
│ ┌────────────────┐ │     │ ┌────────────────┐ │
│ │  Graph Part 0  │ │     │ │  Graph Part 1  │ │
│ │  Vertices 0-N/2│ │     │ │  Vertices N/2-N│ │
│ └───────┬────────┘ │     │ └───────┬────────┘ │
│         │          │     │         │          │
│ ┌───────┴────────┐ │     │ ┌───────┴────────┐ │
│ │  PGAS Runtime  │◄┼─────┼─►  PGAS Runtime  │ │
│ └───────┬────────┘ │     │ └───────┬────────┘ │
│         │          │     │         │          │
│ ┌───────┴────────┐ │     │ ┌───────┴────────┐ │
│ │  CXL Memory    │ │     │ │  CXL Memory    │ │
│ └────────────────┘ │     │ └────────────────┘ │
└─────────┬──────────┘     └─────────┬──────────┘
          │                          │
          └──────────┬───────────────┘
                     │
             ┌───────┴───────┐
             │  CXL Switch   │
             └───────────────┘
```

## Algorithms

### BFS (Breadth-First Search)
Direction-optimizing parallel BFS with frontier-based exploration.

### PageRank
Iterative PageRank with configurable damping factor and convergence threshold.

### Connected Components
Shiloach-Vishkin algorithm with hooking and pointer jumping.

### Triangle Counting
Intersection-based triangle enumeration with sorted adjacency lists.

## Building

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

### Requirements
- C++17 compiler with OpenMP support
- CMake 3.15+
- PGAS library (from memcached-cxl-pgas)

## Usage

### Basic Usage

```bash
# BFS on graph file
./gapbs_pgas -c nodes.conf -g graph.el -a bfs -s 0

# PageRank on synthetic graph
./gapbs_pgas -c nodes.conf -n 1000000 -e 10000000 -a pr

# Connected Components
./gapbs_pgas -c nodes.conf -g graph.el -a cc
```

### Command Line Options

```
-c, --config FILE      PGAS configuration file (required)
-g, --graph FILE       Graph file in edge list format
-n, --nodes N          Vertices for synthetic graphs
-e, --edges E          Edges for synthetic graphs
-a, --algorithm ALG    bfs, pr, sssp, cc, tc (default: bfs)
-s, --source N         Source for BFS/SSSP (default: 0)
-d, --directed         Directed graph
--pr-iters N           PageRank max iterations (default: 100)
--pr-epsilon E         PageRank epsilon (default: 1e-4)
-v, --verify           Verify results
-h, --help             Show help
```

### Graph Formats

**Edge List (.el)**
```
# Comments start with #
0 1
0 2
1 2
2 3
```

## Configuration

Create a PGAS configuration file:

```ini
local_node_id=0
num_nodes=2
node0=192.168.1.10:5000:0x100000000:1073741824
node1=192.168.1.11:5000:0x100000000:1073741824
```

## Graph Partitioning

### Block Partitioning (Default)
```
Node 0: vertices [0, N/2)
Node 1: vertices [N/2, N)
```

### Cyclic Partitioning
```
Node 0: vertices 0, 2, 4, ...
Node 1: vertices 1, 3, 5, ...
```

## Performance Considerations

### Memory Locality
- Local vertex access: ~200 ns
- Remote vertex access: ~1-10 μs (via PGAS)
- Optimize for locality in partitioning

### Load Balancing
- RMAT graphs have skewed degree distribution
- Consider vertex reordering (e.g., by degree)

### Communication
- BFS: O(edges) remote accesses worst case
- PageRank: O(vertices) per iteration
- CC: Depends on graph structure

### Scaling
- Strong scaling: Fixed graph, more nodes
- Weak scaling: Graph grows with nodes

## Example Results

### BFS on Twitter graph (1.2B edges, 2 nodes)
```
PGAS Distributed Graph Statistics:
  Total nodes: 41652230
  Total edges: 1202513046
  Partitions: 2
  Local vertices: 0 - 20826115

BFS Results:
  Max depth: 7
  Time: 23.45 seconds
  Reachable vertices: 41652210 / 41652230
```

### PageRank on Web graph
```
PageRank Results:
  Iterations: 28
  Time: 45.67 seconds
  Top 5 vertices:
    0: 0.00234
    123456: 0.00198
    ...
```

## Distributed Execution

Run on each host:
```bash
# Host 0
./gapbs_pgas -c nodes.conf -g graph.el -a bfs

# Host 1
./gapbs_pgas -c nodes.conf -g graph.el -a bfs
```

All hosts must:
1. Have same graph file or generate same synthetic graph
2. Use same configuration
3. Start at approximately the same time

## Verification

Enable verification with `-v`:
```bash
./gapbs_pgas -c nodes.conf -g graph.el -a bfs -s 0 -v
```

Verifies:
- BFS: Parent pointers form valid tree
- PageRank: Scores sum to 1.0
- CC: Components are valid

## Limitations

1. **Graph Loading**: All nodes must load entire edge list (then partition)
2. **Synchronization**: Barriers after each algorithm phase
3. **Weighted Graphs**: Limited support (SSSP only)
4. **Dynamic Graphs**: Not supported

## Extending

### Add New Algorithm

1. Create class in `pgas_algorithms.h`
2. Implement `Run()` method
3. Use `graph_.IsLocal()` for local/remote distinction
4. Call `graph_.Barrier()` for synchronization
5. Add to main.cpp dispatch

### Custom Partitioning

Implement in `PGASGraph::PartitionVertices()`:
```cpp
case PartitionScheme::CUSTOM:
    // Your partitioning logic
    break;
```

## References

- [GAPBS](https://github.com/sbeamer/gapbs) - Original benchmark suite
- [CXL Specification](https://www.computeexpresslink.org/)
- Beamer et al., "Direction-Optimizing Breadth-First Search"

## License

Research and experimental use.
