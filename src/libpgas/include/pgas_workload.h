#ifndef PGAS_WORKLOAD_H
#define PGAS_WORKLOAD_H

#include "pgas.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * PGAS Workload Abstraction Interface
 *
 * This interface provides a common abstraction for different workloads
 * (graph analytics, key-value stores, scientific computing, etc.) to
 * integrate with the PGAS runtime.
 */

// Workload types
typedef enum {
    PGAS_WORKLOAD_GRAPH = 0,      // Graph analytics (GAPBS, etc.)
    PGAS_WORKLOAD_KV = 1,         // Key-Value stores (Memcached, Redis, etc.)
    PGAS_WORKLOAD_ARRAY = 2,      // Dense array computations (BLAS, etc.)
    PGAS_WORKLOAD_SPARSE = 3,     // Sparse matrix computations
    PGAS_WORKLOAD_CUSTOM = 4      // Custom workload
} pgas_workload_type_t;

// Workload state
typedef enum {
    PGAS_WORKLOAD_STATE_UNINITIALIZED = 0,
    PGAS_WORKLOAD_STATE_INITIALIZED = 1,
    PGAS_WORKLOAD_STATE_RUNNING = 2,
    PGAS_WORKLOAD_STATE_PAUSED = 3,
    PGAS_WORKLOAD_STATE_COMPLETED = 4,
    PGAS_WORKLOAD_STATE_ERROR = 5
} pgas_workload_state_t;

// Partitioning schemes - use pgas_partition_t from pgas.h
typedef pgas_partition_t pgas_partition_scheme_t;

// Workload configuration
typedef struct {
    const char* name;              // Workload name
    pgas_workload_type_t type;     // Workload type
    pgas_partition_scheme_t partition; // Partitioning scheme
    size_t data_size;              // Total data size
    size_t chunk_size;             // Per-node chunk size
    int num_threads;               // Threads per node
    int num_iterations;            // Number of iterations/rounds
    void* workload_specific;       // Workload-specific config
} pgas_workload_config_t;

// Workload statistics
typedef struct {
    uint64_t iterations_completed;
    uint64_t operations_performed;
    uint64_t bytes_processed;
    double elapsed_time_sec;
    double throughput;             // ops/sec or bytes/sec depending on workload
    pgas_stats_t pgas_stats;       // Underlying PGAS statistics
} pgas_workload_stats_t;

// Forward declaration of workload structure
typedef struct pgas_workload pgas_workload_t;

/*
 * Workload interface - function pointers for workload operations
 */
typedef struct {
    // Initialize workload-specific data structures
    int (*init)(pgas_workload_t* workload, pgas_context_t* ctx,
                const pgas_workload_config_t* config);

    // Load data (from file, network, etc.)
    int (*load_data)(pgas_workload_t* workload, const char* source);

    // Partition data across PGAS nodes
    int (*partition)(pgas_workload_t* workload, pgas_partition_scheme_t scheme);

    // Run one iteration of the workload
    int (*run_iteration)(pgas_workload_t* workload);

    // Run the complete workload
    int (*run)(pgas_workload_t* workload);

    // Synchronize with other nodes
    int (*sync)(pgas_workload_t* workload);

    // Check convergence (for iterative algorithms)
    int (*check_convergence)(pgas_workload_t* workload, double* residual);

    // Get results
    int (*get_results)(pgas_workload_t* workload, void* results, size_t* size);

    // Get statistics
    int (*get_stats)(pgas_workload_t* workload, pgas_workload_stats_t* stats);

    // Cleanup workload resources
    void (*finalize)(pgas_workload_t* workload);

} pgas_workload_ops_t;

/*
 * Base workload structure
 */
struct pgas_workload {
    const char* name;
    pgas_workload_type_t type;
    pgas_workload_state_t state;
    pgas_context_t* ctx;
    pgas_workload_config_t config;
    pgas_workload_stats_t stats;
    const pgas_workload_ops_t* ops;
    void* private_data;            // Workload-specific private data
};

/*
 * Workload management functions
 */

// Create a new workload instance
pgas_workload_t* pgas_workload_create(pgas_workload_type_t type, const char* name);

// Initialize workload with PGAS context and configuration
int pgas_workload_init(pgas_workload_t* workload, pgas_context_t* ctx,
                       const pgas_workload_config_t* config);

// Load data from source
int pgas_workload_load(pgas_workload_t* workload, const char* source);

// Partition data across nodes
int pgas_workload_partition(pgas_workload_t* workload, pgas_partition_scheme_t scheme);

// Run the workload
int pgas_workload_run(pgas_workload_t* workload);

// Run single iteration
int pgas_workload_step(pgas_workload_t* workload);

// Synchronize across nodes
int pgas_workload_sync(pgas_workload_t* workload);

// Get workload statistics
int pgas_workload_get_stats(pgas_workload_t* workload, pgas_workload_stats_t* stats);

// Destroy workload instance
void pgas_workload_destroy(pgas_workload_t* workload);

/*
 * Workload registration system
 */

// Register a custom workload type
int pgas_workload_register(const char* name, pgas_workload_type_t type,
                           const pgas_workload_ops_t* ops);

// Get registered workload by name
const pgas_workload_ops_t* pgas_workload_get_ops(const char* name);

// List registered workloads
int pgas_workload_list(char** names, int* count);

// Print workload statistics (convenience function)
void pgas_workload_print_stats(pgas_workload_t* workload);

/*
 * Helper macros for workload implementation
 */

#define PGAS_WORKLOAD_DEFINE(name, type_enum, ops_struct) \
    static __attribute__((constructor)) void _pgas_register_##name(void) { \
        pgas_workload_register(#name, type_enum, &ops_struct); \
    }

#define PGAS_WORKLOAD_CHECK_STATE(workload, expected_state) \
    do { \
        if ((workload)->state != (expected_state)) { \
            return -1; \
        } \
    } while(0)

#define PGAS_WORKLOAD_SET_STATE(workload, new_state) \
    do { \
        (workload)->state = (new_state); \
    } while(0)

/*
 * Distributed data structure helpers
 */

// Calculate local portion of globally distributed array
static inline size_t pgas_local_portion(size_t total_size, uint16_t num_nodes,
                                         uint16_t node_id) {
    size_t base = total_size / num_nodes;
    size_t remainder = total_size % num_nodes;
    return base + (node_id < remainder ? 1 : 0);
}

// Calculate offset for node's portion in block distribution
static inline size_t pgas_block_offset(size_t total_size, uint16_t num_nodes,
                                        uint16_t node_id) {
    size_t base = total_size / num_nodes;
    size_t remainder = total_size % num_nodes;
    if (node_id < remainder) {
        return node_id * (base + 1);
    } else {
        return remainder * (base + 1) + (node_id - remainder) * base;
    }
}

// Get owner node for element in block distribution
static inline uint16_t pgas_block_owner(size_t index, size_t total_size,
                                         uint16_t num_nodes) {
    size_t base = total_size / num_nodes;
    size_t remainder = total_size % num_nodes;
    size_t threshold = remainder * (base + 1);

    if (index < threshold) {
        return (uint16_t)(index / (base + 1));
    } else {
        return (uint16_t)(remainder + (index - threshold) / base);
    }
}

// Get owner node for element in cyclic distribution
static inline uint16_t pgas_cyclic_owner(size_t index, uint16_t num_nodes) {
    return (uint16_t)(index % num_nodes);
}

// Get owner node for element in hash distribution
static inline uint16_t pgas_hash_owner(const void* key, size_t key_len,
                                        uint16_t num_nodes) {
    // Simple FNV-1a hash
    uint64_t hash = 14695981039346656037ULL;
    const uint8_t* data = (const uint8_t*)key;
    for (size_t i = 0; i < key_len; i++) {
        hash ^= data[i];
        hash *= 1099511628211ULL;
    }
    return (uint16_t)(hash % num_nodes);
}

#ifdef __cplusplus
}
#endif

#endif // PGAS_WORKLOAD_H
