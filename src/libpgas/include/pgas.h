#ifndef PGAS_H
#define PGAS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// PGAS Configuration
#define PGAS_MAX_NODES 16
#define PGAS_PAGE_SIZE 4096
#define PGAS_CACHE_LINE_SIZE 64
#define PGAS_DEFAULT_SEGMENT_SIZE (1ULL << 30)  // 1GB

// Memory affinity hints
typedef enum {
    PGAS_AFFINITY_LOCAL = 0,     // Prefer local CXL memory
    PGAS_AFFINITY_REMOTE = 1,    // Prefer remote CXL memory
    PGAS_AFFINITY_INTERLEAVE = 2, // Interleave across nodes
    PGAS_AFFINITY_REPLICATE = 3   // Replicate for read-heavy
} pgas_affinity_t;

// Memory consistency models
typedef enum {
    PGAS_CONSISTENCY_RELAXED = 0,
    PGAS_CONSISTENCY_RELEASE = 1,
    PGAS_CONSISTENCY_ACQUIRE = 2,
    PGAS_CONSISTENCY_SEQ_CST = 3
} pgas_consistency_t;

// Global pointer structure for PGAS addressing
typedef struct {
    uint16_t node_id;      // Target node (0 = local)
    uint16_t segment_id;   // Memory segment
    uint32_t flags;        // Access flags
    uint64_t offset;       // Offset within segment
} pgas_ptr_t;

// Memory segment descriptor
typedef struct {
    uint64_t base_addr;    // Local virtual address
    uint64_t cxl_addr;     // CXL physical address
    size_t size;           // Segment size
    uint16_t owner_node;   // Owner node ID
    pgas_affinity_t affinity;
    bool is_mapped;
    bool is_shared;
} pgas_segment_t;

// Node information
typedef struct {
    uint16_t node_id;
    char hostname[64];
    uint32_t ip_addr;
    uint16_t port;
    bool is_local;
    bool is_active;
    uint64_t cxl_base;     // Base CXL address for this node
    size_t cxl_size;       // Total CXL memory size
} pgas_node_t;

// PGAS runtime context
typedef struct {
    uint16_t local_node_id;
    uint16_t num_nodes;
    pgas_node_t nodes[PGAS_MAX_NODES];
    pgas_segment_t* segments;
    size_t num_segments;
    void* comm_handle;     // Communication layer handle
    void* cxl_handle;      // CXL device handle
} pgas_context_t;

// Initialization and finalization
int pgas_init(pgas_context_t* ctx, const char* config_file);
void pgas_finalize(pgas_context_t* ctx);

// Memory allocation
pgas_ptr_t pgas_alloc(pgas_context_t* ctx, size_t size, pgas_affinity_t affinity);
pgas_ptr_t pgas_alloc_on_node(pgas_context_t* ctx, size_t size, uint16_t node_id);
void pgas_free(pgas_context_t* ctx, pgas_ptr_t ptr);

// Memory access - local operations
void* pgas_local_ptr(pgas_context_t* ctx, pgas_ptr_t gptr);
bool pgas_is_local(pgas_context_t* ctx, pgas_ptr_t gptr);

// Memory access - remote operations
int pgas_get(pgas_context_t* ctx, void* dest, pgas_ptr_t src, size_t size);
int pgas_put(pgas_context_t* ctx, pgas_ptr_t dest, const void* src, size_t size);
int pgas_get_nb(pgas_context_t* ctx, void* dest, pgas_ptr_t src, size_t size, int* handle);
int pgas_put_nb(pgas_context_t* ctx, pgas_ptr_t dest, const void* src, size_t size, int* handle);

// Atomic operations
uint64_t pgas_atomic_fetch_add(pgas_context_t* ctx, pgas_ptr_t ptr, uint64_t value);
uint64_t pgas_atomic_fetch_and(pgas_context_t* ctx, pgas_ptr_t ptr, uint64_t value);
uint64_t pgas_atomic_fetch_or(pgas_context_t* ctx, pgas_ptr_t ptr, uint64_t value);
uint64_t pgas_atomic_cas(pgas_context_t* ctx, pgas_ptr_t ptr, uint64_t expected, uint64_t desired);

// Synchronization
void pgas_fence(pgas_context_t* ctx, pgas_consistency_t consistency);
void pgas_barrier(pgas_context_t* ctx);
int pgas_wait(pgas_context_t* ctx, int handle);
int pgas_wait_all(pgas_context_t* ctx);

// Utility functions
pgas_ptr_t pgas_null_ptr(void);
bool pgas_ptr_is_null(pgas_ptr_t ptr);
bool pgas_ptr_equal(pgas_ptr_t a, pgas_ptr_t b);
uint16_t pgas_ptr_node(pgas_ptr_t ptr);
pgas_ptr_t pgas_ptr_add(pgas_ptr_t ptr, size_t offset);

// Node management
uint16_t pgas_my_node(pgas_context_t* ctx);
uint16_t pgas_num_nodes(pgas_context_t* ctx);
const pgas_node_t* pgas_get_node_info(pgas_context_t* ctx, uint16_t node_id);

// Statistics
typedef struct {
    uint64_t local_reads;
    uint64_t local_writes;
    uint64_t remote_reads;
    uint64_t remote_writes;
    uint64_t atomics;
    uint64_t barriers;
    uint64_t bytes_transferred;
    double avg_latency_us;
} pgas_stats_t;

void pgas_get_stats(pgas_context_t* ctx, pgas_stats_t* stats);
void pgas_reset_stats(pgas_context_t* ctx);

// =============================================================================
// Workload-specific tuning configuration
// =============================================================================

// Prefetch modes for different access patterns
typedef enum {
    PGAS_PREFETCH_NONE = 0,       // No prefetching
    PGAS_PREFETCH_SEQUENTIAL = 1, // Sequential streaming access
    PGAS_PREFETCH_STRIDED = 2,    // Strided access pattern
    PGAS_PREFETCH_AGGRESSIVE = 3, // Aggressive prefetch (high BW)
    PGAS_PREFETCH_NEIGHBOR_LIST = 4 // MD neighbor-list driven
} pgas_prefetch_mode_t;

// Partition schemes for data distribution
typedef enum {
    PGAS_PARTITION_BLOCK = 0,        // Contiguous blocks
    PGAS_PARTITION_CYCLIC = 1,       // Round-robin
    PGAS_PARTITION_BLOCK_CYCLIC = 2, // Block-cyclic (MD domains)
    PGAS_PARTITION_HASH = 3,         // Hash-based
    PGAS_PARTITION_CUSTOM = 4        // User-defined
} pgas_partition_t;

// Workload tuning configuration
typedef struct {
    // Memory configuration
    pgas_affinity_t memory_affinity;    // Memory placement policy
    pgas_partition_t partition_scheme;  // Data partitioning
    bool cache_line_align;              // Align to cache lines
    bool numa_bind;                     // NUMA-aware binding

    // Transfer optimization
    size_t batch_size;                  // Batch remote ops (default: 64)
    size_t transfer_size;               // Bulk transfer size (default: 4KB)
    pgas_prefetch_mode_t prefetch_mode; // Prefetch strategy

    // Consistency
    pgas_consistency_t consistency;     // Memory consistency model

    // Threading
    int num_threads;                    // Worker threads per node

    // Bandwidth hints
    bool bandwidth_priority;            // Prioritize BW over latency
    bool async_transfer;                // Enable async transfers

    // Workload-specific extensions (opaque pointer)
    void* workload_config;
} pgas_tuning_t;

// Predefined tuning profiles
typedef enum {
    PGAS_PROFILE_DEFAULT = 0,
    PGAS_PROFILE_MCF = 1,      // SPEC CPU mcf - pointer chasing
    PGAS_PROFILE_LLAMA = 2,    // LLM inference - bandwidth bound
    PGAS_PROFILE_GROMACS = 3,  // Molecular dynamics - neighbor list
    PGAS_PROFILE_GRAPH = 4,    // Graph analytics (BFS, PageRank)
    PGAS_PROFILE_CUSTOM = 99
} pgas_profile_t;

// Tuning API
int pgas_set_tuning(pgas_context_t* ctx, const pgas_tuning_t* tuning);
int pgas_get_tuning(pgas_context_t* ctx, pgas_tuning_t* tuning);
int pgas_load_profile(pgas_context_t* ctx, pgas_profile_t profile);
const pgas_tuning_t* pgas_get_default_tuning(pgas_profile_t profile);

#ifdef __cplusplus
}
#endif

#endif // PGAS_H
