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

#ifdef __cplusplus
}
#endif

#endif // PGAS_H
