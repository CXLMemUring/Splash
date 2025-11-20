#ifndef MEMCACHED_INTERCEPTOR_H
#define MEMCACHED_INTERCEPTOR_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "pgas.h"

#ifdef __cplusplus
extern "C" {
#endif

// Memcached operation types
typedef enum {
    MC_OP_GET = 0,
    MC_OP_SET = 1,
    MC_OP_ADD = 2,
    MC_OP_REPLACE = 3,
    MC_OP_DELETE = 4,
    MC_OP_INCR = 5,
    MC_OP_DECR = 6,
    MC_OP_APPEND = 7,
    MC_OP_PREPEND = 8,
    MC_OP_CAS = 9,
    MC_OP_GETS = 10,
    MC_OP_TOUCH = 11,
    MC_OP_GAT = 12,      // Get and touch
    MC_OP_FLUSH = 13,
    MC_OP_STATS = 14,
    MC_OP_UNKNOWN = 255
} mc_op_type_t;

// Request routing decision
typedef enum {
    MC_ROUTE_LOCAL = 0,      // Handle locally
    MC_ROUTE_REMOTE = 1,     // Forward to remote node
    MC_ROUTE_CXL_LOCAL = 2,  // Use local CXL memory
    MC_ROUTE_CXL_REMOTE = 3, // Use remote CXL memory
    MC_ROUTE_REPLICATE = 4   // Replicate to all nodes
} mc_route_t;

// Intercepted request structure
typedef struct {
    mc_op_type_t op;
    char* key;
    size_t key_len;
    void* value;
    size_t value_len;
    uint32_t flags;
    uint32_t exptime;
    uint64_t cas_unique;
    uint64_t delta;          // For incr/decr
} mc_request_t;

// Intercepted response structure
typedef struct {
    bool success;
    void* value;
    size_t value_len;
    uint32_t flags;
    uint64_t cas_unique;
    char* error_msg;
} mc_response_t;

// Item metadata for CXL storage
typedef struct {
    uint64_t key_hash;
    uint16_t key_len;
    uint32_t value_len;
    uint32_t flags;
    uint32_t exptime;
    uint64_t cas_unique;
    pgas_ptr_t data_ptr;     // PGAS pointer to actual data
    uint16_t owner_node;
    bool is_locked;
    uint64_t last_access;
} mc_item_meta_t;

// Hash table entry for distributed item lookup
typedef struct {
    uint64_t key_hash;
    pgas_ptr_t meta_ptr;     // PGAS pointer to metadata
    struct mc_hash_entry* next;
} mc_hash_entry_t;

// Interceptor configuration
typedef struct {
    // Routing policy
    bool enable_cxl_disaggregation;
    bool enable_replication;
    int replication_factor;

    // Memory allocation
    size_t local_cache_size;      // Local DRAM cache
    size_t cxl_memory_size;       // CXL memory per node
    float cxl_allocation_ratio;   // 0.0-1.0

    // Performance tuning
    int prefetch_depth;
    bool enable_batching;
    int batch_size;
    int batch_timeout_us;

    // Consistency
    pgas_consistency_t consistency_model;
    bool enable_write_through;

    // Hash table
    size_t hash_table_size;
    int hash_seed;
} mc_interceptor_config_t;

// Interceptor context
typedef struct {
    pgas_context_t* pgas_ctx;
    mc_interceptor_config_t config;

    // Distributed hash table
    mc_hash_entry_t** hash_table;
    size_t hash_table_size;
    pgas_ptr_t remote_hash_table;

    // Local cache
    void* local_cache;
    size_t local_cache_used;

    // Statistics
    uint64_t requests_total;
    uint64_t requests_local;
    uint64_t requests_remote;
    uint64_t cache_hits;
    uint64_t cache_misses;
    uint64_t cxl_reads;
    uint64_t cxl_writes;

    // BPF program handles
    void* bpf_skel;
    int uprobe_fds[32];
    int num_uprobes;
} mc_interceptor_t;

// Initialization and cleanup
int mc_interceptor_init(mc_interceptor_t** interceptor, pgas_context_t* pgas_ctx,
                       const mc_interceptor_config_t* config);
void mc_interceptor_finalize(mc_interceptor_t* interceptor);

// BPF program management
int mc_interceptor_load_bpf(mc_interceptor_t* interceptor, const char* memcached_path);
int mc_interceptor_attach_uprobes(mc_interceptor_t* interceptor);
int mc_interceptor_detach_uprobes(mc_interceptor_t* interceptor);

// Request handling (called from BPF)
mc_route_t mc_determine_route(mc_interceptor_t* interceptor, const mc_request_t* req);
int mc_handle_request(mc_interceptor_t* interceptor, mc_request_t* req, mc_response_t* resp);

// Key routing
uint16_t mc_route_key_to_node(mc_interceptor_t* interceptor, const char* key, size_t key_len);
uint64_t mc_hash_key(const char* key, size_t key_len);

// Item operations on CXL memory
int mc_item_store(mc_interceptor_t* interceptor, const mc_request_t* req, pgas_ptr_t* item_ptr);
int mc_item_fetch(mc_interceptor_t* interceptor, const char* key, size_t key_len, mc_response_t* resp);
int mc_item_delete(mc_interceptor_t* interceptor, const char* key, size_t key_len);
int mc_item_touch(mc_interceptor_t* interceptor, const char* key, size_t key_len, uint32_t exptime);

// Atomic operations
int mc_item_incr_decr(mc_interceptor_t* interceptor, const char* key, size_t key_len,
                      uint64_t delta, bool incr, uint64_t* new_value);
int mc_item_cas(mc_interceptor_t* interceptor, const mc_request_t* req,
                uint64_t cas_unique, mc_response_t* resp);

// Cache management
int mc_cache_insert(mc_interceptor_t* interceptor, uint64_t key_hash, void* value, size_t len);
void* mc_cache_lookup(mc_interceptor_t* interceptor, uint64_t key_hash, size_t* len);
int mc_cache_evict(mc_interceptor_t* interceptor, size_t needed);

// Replication
int mc_replicate_item(mc_interceptor_t* interceptor, const mc_request_t* req, uint16_t* nodes, int count);
int mc_sync_replicas(mc_interceptor_t* interceptor, const char* key, size_t key_len);

// Statistics
typedef struct {
    uint64_t total_requests;
    uint64_t get_requests;
    uint64_t set_requests;
    uint64_t delete_requests;
    uint64_t local_hits;
    uint64_t remote_hits;
    uint64_t cache_hits;
    uint64_t cache_misses;
    uint64_t cxl_bytes_read;
    uint64_t cxl_bytes_written;
    double avg_latency_us;
    double p99_latency_us;
} mc_interceptor_stats_t;

void mc_interceptor_get_stats(mc_interceptor_t* interceptor, mc_interceptor_stats_t* stats);
void mc_interceptor_reset_stats(mc_interceptor_t* interceptor);
void mc_interceptor_print_stats(mc_interceptor_t* interceptor);

// Utility
const char* mc_op_to_string(mc_op_type_t op);
const char* mc_route_to_string(mc_route_t route);

#ifdef __cplusplus
}
#endif

#endif // MEMCACHED_INTERCEPTOR_H
