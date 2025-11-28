#ifndef PGAS_GRAPH_H_
#define PGAS_GRAPH_H_

#include <cstdint>
#include <vector>
#include <algorithm>
#include <iostream>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

// Include PGAS abstraction
extern "C" {
#include <pgas/pgas.h>
#include <pgas/cxl_memory.h>
}

// Type definitions
typedef int64_t NodeID;
typedef int64_t EdgeID;
typedef float ScoreT;
typedef int64_t SGOffset;

// Weighted edge
template <typename WeightT = float>
struct WeightedEdge {
    NodeID dest;
    WeightT weight;

    WeightedEdge() : dest(-1), weight(0) {}
    WeightedEdge(NodeID d, WeightT w) : dest(d), weight(w) {}
};

// Partitioning schemes
enum class PartitionScheme {
    BLOCK,          // Contiguous blocks of vertices
    CYCLIC,         // Round-robin assignment
    EDGE_CUT,       // Minimize edge cuts
    VERTEX_CUT      // Minimize vertex cuts
};

// Graph partition metadata
struct PartitionInfo {
    uint16_t node_id;
    NodeID start_vertex;
    NodeID end_vertex;
    NodeID local_count;        // Number of local vertices
    EdgeID num_local_edges;
    EdgeID num_remote_edges;
    pgas_ptr_t index_ptr;      // PGAS pointer to index array
    pgas_ptr_t neighbors_ptr;  // PGAS pointer to neighbors array
};

// Well-known offsets in PGAS region for graph metadata
// Use high offsets (1GB+) to avoid conflicting with allocator heap at low addresses
// Each node stores: [metadata header][index array][neighbors array]
static const size_t GRAPH_METADATA_OFFSET = 0x40000000;  // 1GB offset
static const size_t GRAPH_INDEX_OFFSET = 0x40100000;     // 1GB + 1MB offset
static const size_t GRAPH_NEIGHBORS_OFFSET = 0x50000000; // 1.25GB offset

// Distributed CSR Graph with PGAS support
template <typename DestT = NodeID>
class PGASGraph {
public:
    PGASGraph() : pgas_ctx_(nullptr), num_nodes_(0), num_edges_(0),
                  directed_(false), num_partitions_(0) {}

    ~PGASGraph() {
        Release();
    }

    // Initialize with PGAS context
    void Init(pgas_context_t* ctx) {
        pgas_ctx_ = ctx;
        num_partitions_ = pgas_num_nodes(ctx);
        local_node_ = pgas_my_node(ctx);
        partitions_.resize(num_partitions_);
    }

    // Build distributed graph from edge list
    void BuildFromEdgeList(const std::vector<std::pair<NodeID, DestT>>& edges,
                          NodeID num_nodes, bool directed = false,
                          PartitionScheme scheme = PartitionScheme::BLOCK) {
        num_nodes_ = num_nodes;
        directed_ = directed;

        // Count edges
        num_edges_ = directed ? edges.size() : edges.size() * 2;

        // Partition vertices across nodes
        PartitionVertices(scheme);

        // Build local CSR structure
        BuildLocalCSR(edges);

        // Synchronize across all nodes
        pgas_barrier(pgas_ctx_);
    }

    // Access methods
    NodeID num_nodes() const { return num_nodes_; }
    EdgeID num_edges() const { return num_edges_; }
    bool directed() const { return directed_; }
    uint16_t num_partitions() const { return num_partitions_; }

    // Check if vertex is local
    bool IsLocal(NodeID v) const {
        return v >= partitions_[local_node_].start_vertex &&
               v < partitions_[local_node_].end_vertex;
    }

    // Get owner node for vertex
    uint16_t GetOwner(NodeID v) const {
        for (uint16_t i = 0; i < num_partitions_; i++) {
            if (v >= partitions_[i].start_vertex &&
                v < partitions_[i].end_vertex) {
                return i;
            }
        }
        return 0;
    }

    // Get local vertex range
    NodeID LocalStart() const { return partitions_[local_node_].start_vertex; }
    NodeID LocalEnd() const { return partitions_[local_node_].end_vertex; }
    NodeID LocalCount() const { return LocalEnd() - LocalStart(); }

    // Get out-degree of vertex
    int64_t OutDegree(NodeID v) const {
        if (IsLocal(v)) {
            NodeID local_v = v - LocalStart();
            return local_index_[local_v + 1] - local_index_[local_v];
        } else {
            // Remote access
            return GetRemoteDegree(v);
        }
    }

    // Iterate over neighbors (local vertex)
    class NeighborIterator {
    public:
        NeighborIterator(DestT* ptr) : ptr_(ptr) {}
        DestT operator*() const { return *ptr_; }
        NeighborIterator& operator++() { ++ptr_; return *this; }
        bool operator!=(const NeighborIterator& other) const {
            return ptr_ != other.ptr_;
        }
    private:
        DestT* ptr_;
    };

    class Neighborhood {
    public:
        Neighborhood(DestT* begin, DestT* end) : begin_(begin), end_(end) {}
        NeighborIterator begin() { return NeighborIterator(begin_); }
        NeighborIterator end() { return NeighborIterator(end_); }
        size_t size() const { return end_ - begin_; }
    private:
        DestT* begin_;
        DestT* end_;
    };

    // Get neighbors of local vertex
    Neighborhood OutNeighbors(NodeID v) {
        if (!IsLocal(v)) {
            std::cerr << "Error: OutNeighbors called on remote vertex " << v << std::endl;
            return Neighborhood(nullptr, nullptr);
        }
        NodeID local_v = v - LocalStart();
        return Neighborhood(
            &local_neighbors_[local_index_[local_v]],
            &local_neighbors_[local_index_[local_v + 1]]
        );
    }

    // Get neighbors of remote vertex (fetches from remote node)
    std::vector<DestT> GetRemoteNeighbors(NodeID v) {
        uint16_t owner = GetOwner(v);
        PartitionInfo& part = partitions_[owner];

        NodeID remote_v = v - part.start_vertex;

        // Construct PGAS pointers to remote node's data at well-known offsets
        pgas_ptr_t remote_index_ptr;
        remote_index_ptr.node_id = owner;
        remote_index_ptr.segment_id = 0;
        remote_index_ptr.flags = 0;
        remote_index_ptr.offset = GRAPH_INDEX_OFFSET;

        pgas_ptr_t remote_neighbors_ptr;
        remote_neighbors_ptr.node_id = owner;
        remote_neighbors_ptr.segment_id = 0;
        remote_neighbors_ptr.flags = 0;
        remote_neighbors_ptr.offset = GRAPH_NEIGHBORS_OFFSET;

        // Fetch index entries
        SGOffset start_offset, end_offset;
        pgas_ptr_t idx_ptr = pgas_ptr_add(remote_index_ptr, remote_v * sizeof(SGOffset));
        pgas_get(pgas_ctx_, &start_offset, idx_ptr, sizeof(SGOffset));

        idx_ptr = pgas_ptr_add(remote_index_ptr, (remote_v + 1) * sizeof(SGOffset));
        pgas_get(pgas_ctx_, &end_offset, idx_ptr, sizeof(SGOffset));

        // Validate offsets to prevent bad allocations
        if (start_offset < 0 || end_offset < start_offset || end_offset > 1000000000LL) {
            std::cerr << "Warning: Invalid remote index values for vertex " << v
                      << " on node " << owner << ": start=" << start_offset
                      << ", end=" << end_offset << std::endl;
            return std::vector<DestT>();
        }

        // Fetch neighbors
        size_t num_neighbors = end_offset - start_offset;
        std::vector<DestT> neighbors(num_neighbors);

        if (num_neighbors > 0) {
            pgas_ptr_t neigh_ptr = pgas_ptr_add(remote_neighbors_ptr, start_offset * sizeof(DestT));
            pgas_get(pgas_ctx_, neighbors.data(), neigh_ptr, num_neighbors * sizeof(DestT));
        }

        return neighbors;
    }

    // Allocate vertex property array distributed across nodes
    template <typename T>
    pgas_ptr_t AllocVertexArray(T init_value = T()) {
        size_t local_size = LocalCount() * sizeof(T);
        pgas_ptr_t ptr = pgas_alloc(pgas_ctx_, local_size, PGAS_AFFINITY_LOCAL);

        if (!pgas_ptr_is_null(ptr)) {
            // Initialize local portion
            T* local_ptr = (T*)pgas_local_ptr(pgas_ctx_, ptr);
            for (NodeID i = 0; i < LocalCount(); i++) {
                local_ptr[i] = init_value;
            }
        }

        return ptr;
    }

    // Get value from vertex array
    template <typename T>
    T GetVertexValue(pgas_ptr_t array_ptr, NodeID v) {
        uint16_t owner = GetOwner(v);
        NodeID local_v = v - partitions_[owner].start_vertex;

        if (owner == local_node_) {
            T* local_ptr = (T*)pgas_local_ptr(pgas_ctx_, array_ptr);
            return local_ptr[local_v];
        } else {
            T value;
            pgas_ptr_t val_ptr = pgas_ptr_add(array_ptr, local_v * sizeof(T));
            // Need to get the remote array pointer for this node
            // This is simplified - real impl would track per-node arrays
            pgas_get(pgas_ctx_, &value, val_ptr, sizeof(T));
            return value;
        }
    }

    // Set value in vertex array
    template <typename T>
    void SetVertexValue(pgas_ptr_t array_ptr, NodeID v, T value) {
        uint16_t owner = GetOwner(v);
        NodeID local_v = v - partitions_[owner].start_vertex;

        if (owner == local_node_) {
            T* local_ptr = (T*)pgas_local_ptr(pgas_ctx_, array_ptr);
            local_ptr[local_v] = value;
        } else {
            pgas_ptr_t val_ptr = pgas_ptr_add(array_ptr, local_v * sizeof(T));
            pgas_put(pgas_ctx_, val_ptr, &value, sizeof(T));
        }
    }

    // Atomic compare-and-swap on vertex value
    template <typename T>
    T AtomicCAS(pgas_ptr_t array_ptr, NodeID v, T expected, T desired) {
        uint16_t owner = GetOwner(v);
        NodeID local_v = v - partitions_[owner].start_vertex;
        pgas_ptr_t val_ptr = pgas_ptr_add(array_ptr, local_v * sizeof(T));

        return (T)pgas_atomic_cas(pgas_ctx_, val_ptr, (uint64_t)expected, (uint64_t)desired);
    }

    // Print graph statistics
    void PrintStats() const {
        std::cout << "PGAS Distributed Graph Statistics:" << std::endl;
        std::cout << "  Total nodes: " << num_nodes_ << std::endl;
        std::cout << "  Total edges: " << num_edges_ << std::endl;
        std::cout << "  Directed: " << (directed_ ? "yes" : "no") << std::endl;
        std::cout << "  Partitions: " << num_partitions_ << std::endl;
        std::cout << "  Local node: " << local_node_ << std::endl;
        std::cout << "  Local vertices: " << LocalStart() << " - " << LocalEnd() << std::endl;
        std::cout << "  Local edges: " << local_neighbors_.size() << std::endl;
    }

    // Get partition info
    const PartitionInfo& GetPartition(uint16_t node) const {
        return partitions_[node];
    }

    // Synchronization
    void Barrier() {
        pgas_barrier(pgas_ctx_);
    }

    void Fence() {
        pgas_fence(pgas_ctx_, PGAS_CONSISTENCY_SEQ_CST);
    }

private:
    pgas_context_t* pgas_ctx_;
    NodeID num_nodes_;
    EdgeID num_edges_;
    bool directed_;
    uint16_t num_partitions_;
    uint16_t local_node_;

    // Partition information for all nodes
    std::vector<PartitionInfo> partitions_;

    // Local CSR data
    std::vector<SGOffset> local_index_;
    std::vector<DestT> local_neighbors_;

    void PartitionVertices(PartitionScheme scheme) {
        NodeID vertices_per_node = (num_nodes_ + num_partitions_ - 1) / num_partitions_;

        for (uint16_t i = 0; i < num_partitions_; i++) {
            partitions_[i].node_id = i;

            switch (scheme) {
                case PartitionScheme::BLOCK:
                    partitions_[i].start_vertex = i * vertices_per_node;
                    partitions_[i].end_vertex = std::min((i + 1) * vertices_per_node, num_nodes_);
                    break;

                case PartitionScheme::CYCLIC:
                    // More complex - would need different storage
                    partitions_[i].start_vertex = i * vertices_per_node;
                    partitions_[i].end_vertex = std::min((i + 1) * vertices_per_node, num_nodes_);
                    break;

                default:
                    partitions_[i].start_vertex = i * vertices_per_node;
                    partitions_[i].end_vertex = std::min((i + 1) * vertices_per_node, num_nodes_);
            }
        }
    }

    void BuildLocalCSR(const std::vector<std::pair<NodeID, DestT>>& edges) {
        NodeID local_start = LocalStart();
        NodeID local_end = LocalEnd();
        NodeID local_count = local_end - local_start;

        // Count degrees
        std::vector<SGOffset> degrees(local_count + 1, 0);

        for (const auto& edge : edges) {
            if (edge.first >= local_start && edge.first < local_end) {
                degrees[edge.first - local_start + 1]++;
            }
            if (!directed_ && edge.second >= local_start && edge.second < local_end) {
                // For undirected, also add reverse edge
                NodeID dest_id = edge.second;
                if constexpr (std::is_same_v<DestT, NodeID>) {
                    degrees[dest_id - local_start + 1]++;
                }
            }
        }

        // Compute offsets
        local_index_.resize(local_count + 1);
        local_index_[0] = 0;
        for (NodeID i = 0; i < local_count; i++) {
            local_index_[i + 1] = local_index_[i] + degrees[i + 1];
        }

        // Allocate neighbors
        EdgeID total_local_edges = local_index_[local_count];
        local_neighbors_.resize(total_local_edges);

        // Fill neighbors
        std::vector<SGOffset> current_pos(local_count, 0);

        for (const auto& edge : edges) {
            if (edge.first >= local_start && edge.first < local_end) {
                NodeID local_v = edge.first - local_start;
                SGOffset pos = local_index_[local_v] + current_pos[local_v]++;
                local_neighbors_[pos] = edge.second;
            }
            if (!directed_) {
                NodeID dest_id;
                if constexpr (std::is_same_v<DestT, NodeID>) {
                    dest_id = edge.second;
                    if (dest_id >= local_start && dest_id < local_end) {
                        NodeID local_v = dest_id - local_start;
                        SGOffset pos = local_index_[local_v] + current_pos[local_v]++;
                        local_neighbors_[pos] = edge.first;
                    }
                }
            }
        }

        // Sort neighbors for each vertex
        for (NodeID v = 0; v < local_count; v++) {
            std::sort(local_neighbors_.begin() + local_index_[v],
                     local_neighbors_.begin() + local_index_[v + 1]);
        }

        // Store index and neighbors at well-known offsets in CXL memory
        // This allows remote nodes to access our data without needing pointer exchange
        size_t index_size = (local_count + 1) * sizeof(SGOffset);
        size_t neighbors_size = total_local_edges * sizeof(DestT);

        // Create PGAS pointers at well-known offsets
        partitions_[local_node_].index_ptr.node_id = local_node_;
        partitions_[local_node_].index_ptr.segment_id = 0;
        partitions_[local_node_].index_ptr.flags = 0;
        partitions_[local_node_].index_ptr.offset = GRAPH_INDEX_OFFSET;

        partitions_[local_node_].neighbors_ptr.node_id = local_node_;
        partitions_[local_node_].neighbors_ptr.segment_id = 0;
        partitions_[local_node_].neighbors_ptr.flags = 0;
        partitions_[local_node_].neighbors_ptr.offset = GRAPH_NEIGHBORS_OFFSET;

        // Copy to CXL memory using pgas_put (which handles local puts efficiently)
        std::cout << "  Writing index array (" << index_size << " bytes) to offset 0x"
                  << std::hex << GRAPH_INDEX_OFFSET << std::dec << std::endl;
        pgas_put(pgas_ctx_, partitions_[local_node_].index_ptr,
                local_index_.data(), index_size);
        std::cout << "  Index array written successfully" << std::endl;

        std::cout << "  Writing neighbors array (" << neighbors_size << " bytes) to offset 0x"
                  << std::hex << GRAPH_NEIGHBORS_OFFSET << std::dec << std::endl;
        pgas_put(pgas_ctx_, partitions_[local_node_].neighbors_ptr,
                local_neighbors_.data(), neighbors_size);
        std::cout << "  Neighbors array written successfully" << std::endl;

        partitions_[local_node_].local_count = local_count;
        partitions_[local_node_].num_local_edges = total_local_edges;
    }

    int64_t GetRemoteDegree(NodeID v) const {
        uint16_t owner = GetOwner(v);
        const PartitionInfo& part = partitions_[owner];
        NodeID remote_v = v - part.start_vertex;

        // Construct PGAS pointer to remote node's index at well-known offset
        pgas_ptr_t remote_index_ptr;
        remote_index_ptr.node_id = owner;
        remote_index_ptr.segment_id = 0;
        remote_index_ptr.flags = 0;
        remote_index_ptr.offset = GRAPH_INDEX_OFFSET;

        SGOffset start_offset, end_offset;
        pgas_ptr_t idx_ptr = pgas_ptr_add(remote_index_ptr, remote_v * sizeof(SGOffset));
        pgas_get(pgas_ctx_, &start_offset, idx_ptr, sizeof(SGOffset));

        idx_ptr = pgas_ptr_add(remote_index_ptr, (remote_v + 1) * sizeof(SGOffset));
        pgas_get(pgas_ctx_, &end_offset, idx_ptr, sizeof(SGOffset));

        return end_offset - start_offset;
    }

    void Release() {
        // Note: We use fixed offsets, not dynamic allocation, so no pgas_free needed
        local_index_.clear();
        local_neighbors_.clear();
    }
};

#endif  // PGAS_GRAPH_H_
