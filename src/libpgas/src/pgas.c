#define _GNU_SOURCE
#include "pgas.h"
#include "cxl_memory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>

// Internal communication message types
typedef enum {
    MSG_GET = 1,
    MSG_PUT = 2,
    MSG_GET_RESP = 3,
    MSG_PUT_RESP = 4,
    MSG_ATOMIC_FAA = 5,
    MSG_ATOMIC_CAS = 6,
    MSG_ATOMIC_RESP = 7,
    MSG_BARRIER = 8,
    MSG_BARRIER_RESP = 9,
    MSG_ALLOC = 10,
    MSG_ALLOC_RESP = 11,
    MSG_FREE = 12
} comm_msg_type_t;

// Communication message header
typedef struct {
    uint32_t msg_type;
    uint32_t msg_len;
    uint16_t src_node;
    uint16_t dst_node;
    uint64_t request_id;
} comm_header_t;

// Communication message for remote memory operations
typedef struct {
    comm_header_t header;
    pgas_ptr_t ptr;
    size_t size;
    uint64_t value;  // For atomics
    char data[];     // Flexible array member
} comm_message_t;

// Communication handle
typedef struct {
    int listen_fd;
    volatile int* peer_fds;       /* For sending requests TO peers */
    volatile int* peer_recv_fds;  /* For receiving responses FROM peers (handler writes here) */
    pthread_t listener_thread;
    pthread_mutex_t send_lock;
    pthread_mutex_t peer_lock;  /* protects peer_fds array */
    pthread_mutex_t* recv_locks; /* per-peer locks for recv */
    uint64_t next_request_id;

    // Pending requests
    pthread_mutex_t pending_lock;
    pthread_cond_t pending_cond;
    struct pending_request* pending;
} comm_handle_t;

// Pending request for async operations
struct pending_request {
    uint64_t request_id;
    bool completed;
    void* result;
    size_t result_len;
    struct pending_request* next;
};

// Internal statistics
typedef struct {
    uint64_t local_reads;
    uint64_t local_writes;
    uint64_t remote_reads;
    uint64_t remote_writes;
    uint64_t atomics;
    uint64_t barriers;
    uint64_t bytes_transferred;
    uint64_t total_latency_ns;
    uint64_t num_operations;
} internal_stats_t;

static internal_stats_t* get_stats(pgas_context_t* ctx) {
    static internal_stats_t stats = {0};
    return &stats;
}

// Communication functions
static int comm_init(pgas_context_t* ctx, uint16_t port);
static void comm_finalize(pgas_context_t* ctx);
static int comm_connect_peers(pgas_context_t* ctx);
static int comm_send(pgas_context_t* ctx, uint16_t node_id, void* data, size_t len);
static int comm_recv(pgas_context_t* ctx, uint16_t node_id, void* data, size_t max_len);
static int comm_send_recv(pgas_context_t* ctx, uint16_t node_id,
                          void* req_data, size_t req_len,
                          void* resp_data, size_t resp_len);
static void* comm_listener_thread(void* arg);

// Memory segment management
static int init_segments(pgas_context_t* ctx);
static pgas_segment_t* find_segment(pgas_context_t* ctx, uint16_t node_id, uint64_t offset);
static void* translate_address(pgas_context_t* ctx, pgas_ptr_t ptr);

int pgas_init(pgas_context_t* ctx, const char* config_file) {
    if (!ctx) return -1;

    memset(ctx, 0, sizeof(pgas_context_t));

    // Parse configuration file
    FILE* fp = fopen(config_file, "r");
    if (!fp) {
        fprintf(stderr, "Failed to open config file: %s\n", config_file);
        return -1;
    }

    char line[256];
    int node_idx = 0;

    while (fgets(line, sizeof(line), fp)) {
        if (line[0] == '#' || line[0] == '\n') continue;

        char key[64], value[192];
        if (sscanf(line, "%63[^=]=%191s", key, value) != 2) continue;

        if (strcmp(key, "local_node_id") == 0) {
            ctx->local_node_id = atoi(value);
        } else if (strcmp(key, "num_nodes") == 0) {
            ctx->num_nodes = atoi(value);
        } else if (strncmp(key, "node", 4) == 0) {
            // Parse node configuration: nodeX=hostname:port:cxl_base:cxl_size
            int idx = atoi(key + 4);
            if (idx < PGAS_MAX_NODES) {
                char hostname[64];
                int port;
                uint64_t cxl_base, cxl_size;
                if (sscanf(value, "%63[^:]:%d:%lx:%lu", hostname, &port, &cxl_base, &cxl_size) == 4) {
                    strncpy(ctx->nodes[idx].hostname, hostname, sizeof(ctx->nodes[idx].hostname) - 1);
                    ctx->nodes[idx].node_id = idx;
                    ctx->nodes[idx].port = port;
                    ctx->nodes[idx].cxl_base = cxl_base;
                    ctx->nodes[idx].cxl_size = cxl_size;
                    ctx->nodes[idx].is_local = (idx == ctx->local_node_id);
                    ctx->nodes[idx].is_active = true;

                    // Convert hostname to IP
                    struct in_addr addr;
                    if (inet_aton(hostname, &addr)) {
                        ctx->nodes[idx].ip_addr = addr.s_addr;
                    }
                }
            }
        }
    }

    fclose(fp);

    // Initialize CXL memory
    cxl_handle_t* cxl_handle;
    if (cxl_init(&cxl_handle, NULL) != 0) {
        fprintf(stderr, "Failed to initialize CXL memory\n");
        return -1;
    }
    ctx->cxl_handle = cxl_handle;

    // Update local node's cxl_base to the actual virtual address
    // This is needed because each process has its own virtual address space
    if (cxl_handle->num_devices > 0) {
        ctx->nodes[ctx->local_node_id].cxl_base = cxl_handle->devices[0].base_address;
    }

    // Initialize communication layer
    if (comm_init(ctx, ctx->nodes[ctx->local_node_id].port) != 0) {
        fprintf(stderr, "Failed to initialize communication\n");
        return -1;
    }

    // Connect to peer nodes
    if (comm_connect_peers(ctx) != 0) {
        fprintf(stderr, "Warning: Could not connect to all peers\n");
    }

    // Initialize memory segments
    if (init_segments(ctx) != 0) {
        fprintf(stderr, "Failed to initialize memory segments\n");
        return -1;
    }

    printf("PGAS initialized: node %d of %d\n", ctx->local_node_id, ctx->num_nodes);
    return 0;
}

void pgas_finalize(pgas_context_t* ctx) {
    if (!ctx) return;

    comm_finalize(ctx);

    if (ctx->cxl_handle) {
        cxl_finalize((cxl_handle_t*)ctx->cxl_handle);
    }

    if (ctx->segments) {
        free(ctx->segments);
    }
}

pgas_ptr_t pgas_alloc(pgas_context_t* ctx, size_t size, pgas_affinity_t affinity) {
    uint16_t target_node;

    switch (affinity) {
        case PGAS_AFFINITY_LOCAL:
            target_node = ctx->local_node_id;
            break;
        case PGAS_AFFINITY_REMOTE:
            // Pick a random remote node
            target_node = (ctx->local_node_id + 1) % ctx->num_nodes;
            break;
        case PGAS_AFFINITY_INTERLEAVE:
            // Round-robin across nodes
            {
                static uint16_t next_node = 0;
                target_node = next_node;
                next_node = (next_node + 1) % ctx->num_nodes;
            }
            break;
        default:
            target_node = ctx->local_node_id;
    }

    return pgas_alloc_on_node(ctx, size, target_node);
}

pgas_ptr_t pgas_alloc_on_node(pgas_context_t* ctx, size_t size, uint16_t node_id) {
    pgas_ptr_t result = pgas_null_ptr();

    if (node_id == ctx->local_node_id) {
        // Local allocation
        void* ptr = cxl_alloc((cxl_handle_t*)ctx->cxl_handle, size, PGAS_CACHE_LINE_SIZE);
        if (ptr) {
            result.node_id = node_id;
            result.segment_id = 0;  // Default segment
            result.offset = (uint64_t)ptr - ctx->nodes[node_id].cxl_base;
            result.flags = 0;
        }
    } else {
        // Remote allocation - send request to target node
        comm_message_t msg = {0};
        msg.header.msg_type = MSG_ALLOC;
        msg.header.src_node = ctx->local_node_id;
        msg.header.dst_node = node_id;
        msg.size = size;

        comm_handle_t* comm = (comm_handle_t*)ctx->comm_handle;
        msg.header.request_id = __sync_fetch_and_add(&comm->next_request_id, 1);

        comm_send(ctx, node_id, &msg, sizeof(msg));

        // Wait for response
        comm_message_t resp;
        comm_recv(ctx, node_id, &resp, sizeof(resp));

        if (resp.header.msg_type == MSG_ALLOC_RESP) {
            result = resp.ptr;
        }
    }

    return result;
}

void pgas_free(pgas_context_t* ctx, pgas_ptr_t ptr) {
    if (pgas_ptr_is_null(ptr)) return;

    if (ptr.node_id == ctx->local_node_id) {
        void* local_ptr = translate_address(ctx, ptr);
        if (local_ptr) {
            cxl_free((cxl_handle_t*)ctx->cxl_handle, local_ptr);
        }
    } else {
        // Remote free
        comm_message_t msg = {0};
        msg.header.msg_type = MSG_FREE;
        msg.header.src_node = ctx->local_node_id;
        msg.header.dst_node = ptr.node_id;
        msg.ptr = ptr;

        comm_send(ctx, ptr.node_id, &msg, sizeof(msg));
    }
}

void* pgas_local_ptr(pgas_context_t* ctx, pgas_ptr_t gptr) {
    if (!pgas_is_local(ctx, gptr)) return NULL;
    return translate_address(ctx, gptr);
}

bool pgas_is_local(pgas_context_t* ctx, pgas_ptr_t gptr) {
    return gptr.node_id == ctx->local_node_id;
}

int pgas_get(pgas_context_t* ctx, void* dest, pgas_ptr_t src, size_t size) {
    internal_stats_t* stats = get_stats(ctx);

    if (pgas_is_local(ctx, src)) {
        // Local get
        void* local_ptr = translate_address(ctx, src);
        if (!local_ptr) return -1;

        memcpy(dest, local_ptr, size);
        stats->local_reads++;
    } else {
        // Remote get - use atomic send/recv to ensure response matching
        comm_message_t msg = {0};
        msg.header.msg_type = MSG_GET;
        msg.header.src_node = ctx->local_node_id;
        msg.header.dst_node = src.node_id;
        msg.ptr = src;
        msg.size = size;

        comm_handle_t* comm = (comm_handle_t*)ctx->comm_handle;
        msg.header.request_id = __sync_fetch_and_add(&comm->next_request_id, 1);

        // Allocate response buffer
        comm_message_t* resp = malloc(sizeof(comm_message_t) + size);
        if (!resp) return -1;

        // Send request and receive response atomically
        int ret = comm_send_recv(ctx, src.node_id,
                                  &msg, sizeof(msg),
                                  resp, sizeof(comm_message_t) + size);

        if (ret > 0 && resp->header.msg_type == MSG_GET_RESP) {
            memcpy(dest, resp->data, size);
            stats->remote_reads++;
            stats->bytes_transferred += size;
        } else {
            // Remote get failed - could be peer disconnected
            memset(dest, 0, size);
            free(resp);
            return -1;
        }

        free(resp);
    }

    return 0;
}

int pgas_put(pgas_context_t* ctx, pgas_ptr_t dest, const void* src, size_t size) {
    internal_stats_t* stats = get_stats(ctx);

    if (pgas_is_local(ctx, dest)) {
        // Local put
        void* local_ptr = translate_address(ctx, dest);
        if (!local_ptr) {
            return -1;
        }

        memcpy(local_ptr, src, size);
        cxl_flush(local_ptr, size);
        stats->local_writes++;
    } else {
        // Remote put - use atomic send/recv
        comm_message_t* msg = malloc(sizeof(comm_message_t) + size);
        if (!msg) return -1;

        msg->header.msg_type = MSG_PUT;
        msg->header.src_node = ctx->local_node_id;
        msg->header.dst_node = dest.node_id;
        msg->header.msg_len = sizeof(comm_message_t) + size;
        msg->ptr = dest;
        msg->size = size;
        memcpy(msg->data, src, size);

        comm_handle_t* comm = (comm_handle_t*)ctx->comm_handle;
        msg->header.request_id = __sync_fetch_and_add(&comm->next_request_id, 1);

        // Send request and receive ack atomically
        comm_message_t resp;
        comm_send_recv(ctx, dest.node_id,
                       msg, sizeof(comm_message_t) + size,
                       &resp, sizeof(resp));

        stats->remote_writes++;
        stats->bytes_transferred += size;

        free(msg);
    }

    return 0;
}

uint64_t pgas_atomic_fetch_add(pgas_context_t* ctx, pgas_ptr_t ptr, uint64_t value) {
    internal_stats_t* stats = get_stats(ctx);
    uint64_t result;

    if (pgas_is_local(ctx, ptr)) {
        uint64_t* local_ptr = (uint64_t*)translate_address(ctx, ptr);
        if (!local_ptr) return 0;

        result = __sync_fetch_and_add(local_ptr, value);
    } else {
        // Remote atomic - use atomic send/recv
        comm_message_t msg = {0};
        msg.header.msg_type = MSG_ATOMIC_FAA;
        msg.header.src_node = ctx->local_node_id;
        msg.header.dst_node = ptr.node_id;
        msg.ptr = ptr;
        msg.value = value;

        comm_handle_t* comm = (comm_handle_t*)ctx->comm_handle;
        msg.header.request_id = __sync_fetch_and_add(&comm->next_request_id, 1);

        comm_message_t resp;
        comm_send_recv(ctx, ptr.node_id, &msg, sizeof(msg), &resp, sizeof(resp));

        result = resp.value;
    }

    stats->atomics++;
    return result;
}

uint64_t pgas_atomic_cas(pgas_context_t* ctx, pgas_ptr_t ptr, uint64_t expected, uint64_t desired) {
    internal_stats_t* stats = get_stats(ctx);
    uint64_t result;

    if (pgas_is_local(ctx, ptr)) {
        uint64_t* local_ptr = (uint64_t*)translate_address(ctx, ptr);
        if (!local_ptr) return 0;

        result = __sync_val_compare_and_swap(local_ptr, expected, desired);
    } else {
        // Remote CAS - use atomic send/recv
        comm_message_t msg = {0};
        msg.header.msg_type = MSG_ATOMIC_CAS;
        msg.header.src_node = ctx->local_node_id;
        msg.header.dst_node = ptr.node_id;
        msg.ptr = ptr;
        msg.value = expected;
        msg.size = desired;  // Reuse size field for desired value

        comm_handle_t* comm = (comm_handle_t*)ctx->comm_handle;
        msg.header.request_id = __sync_fetch_and_add(&comm->next_request_id, 1);

        comm_message_t resp;
        comm_send_recv(ctx, ptr.node_id, &msg, sizeof(msg), &resp, sizeof(resp));

        result = resp.value;
    }

    stats->atomics++;
    return result;
}

void pgas_fence(pgas_context_t* ctx, pgas_consistency_t consistency) {
    switch (consistency) {
        case PGAS_CONSISTENCY_RELAXED:
            __asm__ volatile("" ::: "memory");
            break;
        case PGAS_CONSISTENCY_RELEASE:
            __asm__ volatile("sfence" ::: "memory");
            break;
        case PGAS_CONSISTENCY_ACQUIRE:
            __asm__ volatile("lfence" ::: "memory");
            break;
        case PGAS_CONSISTENCY_SEQ_CST:
            __asm__ volatile("mfence" ::: "memory");
            break;
    }
}

void pgas_barrier(pgas_context_t* ctx) {
    internal_stats_t* stats = get_stats(ctx);

    // Simple two-phase barrier
    for (int i = 0; i < ctx->num_nodes; i++) {
        if (i != ctx->local_node_id && ctx->nodes[i].is_active) {
            comm_message_t msg = {0};
            msg.header.msg_type = MSG_BARRIER;
            msg.header.src_node = ctx->local_node_id;
            msg.header.dst_node = i;

            comm_send(ctx, i, &msg, sizeof(msg));
        }
    }

    // Wait for all nodes to reach barrier
    for (int i = 0; i < ctx->num_nodes; i++) {
        if (i != ctx->local_node_id && ctx->nodes[i].is_active) {
            comm_message_t resp;
            comm_recv(ctx, i, &resp, sizeof(resp));
        }
    }

    // Send release
    for (int i = 0; i < ctx->num_nodes; i++) {
        if (i != ctx->local_node_id && ctx->nodes[i].is_active) {
            comm_message_t msg = {0};
            msg.header.msg_type = MSG_BARRIER_RESP;
            msg.header.src_node = ctx->local_node_id;
            msg.header.dst_node = i;

            comm_send(ctx, i, &msg, sizeof(msg));
        }
    }

    stats->barriers++;
}

// Utility functions
pgas_ptr_t pgas_null_ptr(void) {
    pgas_ptr_t ptr = {0xFFFF, 0xFFFF, 0, 0};
    return ptr;
}

bool pgas_ptr_is_null(pgas_ptr_t ptr) {
    return ptr.node_id == 0xFFFF && ptr.segment_id == 0xFFFF;
}

bool pgas_ptr_equal(pgas_ptr_t a, pgas_ptr_t b) {
    return a.node_id == b.node_id &&
           a.segment_id == b.segment_id &&
           a.offset == b.offset;
}

uint16_t pgas_ptr_node(pgas_ptr_t ptr) {
    return ptr.node_id;
}

pgas_ptr_t pgas_ptr_add(pgas_ptr_t ptr, size_t offset) {
    ptr.offset += offset;
    return ptr;
}

uint16_t pgas_my_node(pgas_context_t* ctx) {
    return ctx->local_node_id;
}

uint16_t pgas_num_nodes(pgas_context_t* ctx) {
    return ctx->num_nodes;
}

const pgas_node_t* pgas_get_node_info(pgas_context_t* ctx, uint16_t node_id) {
    if (node_id >= PGAS_MAX_NODES) return NULL;
    return &ctx->nodes[node_id];
}

void pgas_get_stats(pgas_context_t* ctx, pgas_stats_t* stats) {
    internal_stats_t* internal = get_stats(ctx);
    stats->local_reads = internal->local_reads;
    stats->local_writes = internal->local_writes;
    stats->remote_reads = internal->remote_reads;
    stats->remote_writes = internal->remote_writes;
    stats->atomics = internal->atomics;
    stats->barriers = internal->barriers;
    stats->bytes_transferred = internal->bytes_transferred;
    if (internal->num_operations > 0) {
        stats->avg_latency_us = internal->total_latency_ns / internal->num_operations / 1000.0;
    } else {
        stats->avg_latency_us = 0;
    }
}

void pgas_reset_stats(pgas_context_t* ctx) {
    internal_stats_t* stats = get_stats(ctx);
    memset(stats, 0, sizeof(*stats));
}

// =============================================================================
// Workload Tuning Implementation
// =============================================================================

// Global tuning state
static pgas_tuning_t g_current_tuning = {0};
static bool g_tuning_initialized = false;

// Predefined tuning profiles
static const pgas_tuning_t TUNING_DEFAULT = {
    .memory_affinity = PGAS_AFFINITY_LOCAL,
    .partition_scheme = PGAS_PARTITION_BLOCK,
    .cache_line_align = true,
    .numa_bind = false,
    .batch_size = 64,
    .transfer_size = 4096,
    .prefetch_mode = PGAS_PREFETCH_NONE,
    .consistency = PGAS_CONSISTENCY_SEQ_CST,
    .num_threads = 1,
    .bandwidth_priority = false,
    .async_transfer = false,
    .workload_config = NULL
};

// MCF: Pointer-chasing, high cache miss rate, latency sensitive
static const pgas_tuning_t TUNING_MCF = {
    .memory_affinity = PGAS_AFFINITY_LOCAL,      // Keep critical data local
    .partition_scheme = PGAS_PARTITION_BLOCK,    // Spatial locality
    .cache_line_align = true,                    // Reduce false sharing
    .numa_bind = true,                           // NUMA-aware
    .batch_size = 64,                            // Small batches for latency
    .transfer_size = 64,                         // Cache line sized
    .prefetch_mode = PGAS_PREFETCH_AGGRESSIVE,   // Prefetch helps struct traversal
    .consistency = PGAS_CONSISTENCY_RELAXED,     // Read-heavy workload
    .num_threads = 1,                            // Mostly single-threaded
    .bandwidth_priority = false,                 // Latency matters more
    .async_transfer = false,
    .workload_config = NULL
};

// LLAMA: Memory bandwidth bound, sequential weight loading
static const pgas_tuning_t TUNING_LLAMA = {
    .memory_affinity = PGAS_AFFINITY_INTERLEAVE, // Aggregate bandwidth
    .partition_scheme = PGAS_PARTITION_BLOCK,    // Layer-wise distribution
    .cache_line_align = true,
    .numa_bind = true,
    .batch_size = 4096,                          // Large batches
    .transfer_size = 1048576,                    // 1MB transfers
    .prefetch_mode = PGAS_PREFETCH_SEQUENTIAL,   // Streaming access
    .consistency = PGAS_CONSISTENCY_RELAXED,     // Read-only weights
    .num_threads = 8,                            // Parallel weight loading
    .bandwidth_priority = true,                  // Maximize bandwidth
    .async_transfer = true,                      // Overlap compute/transfer
    .workload_config = NULL
};

// GROMACS: Neighbor-list driven, domain decomposition
static const pgas_tuning_t TUNING_GROMACS = {
    .memory_affinity = PGAS_AFFINITY_LOCAL,      // Domain-local data
    .partition_scheme = PGAS_PARTITION_BLOCK_CYCLIC, // 3D domain decomp
    .cache_line_align = true,                    // Force array alignment
    .numa_bind = true,                           // NUMA-aware
    .batch_size = 256,                           // Halo exchange batches
    .transfer_size = 8192,                       // 8KB for particle data
    .prefetch_mode = PGAS_PREFETCH_NEIGHBOR_LIST,// Use neighbor list
    .consistency = PGAS_CONSISTENCY_RELEASE,     // Sync at boundaries
    .num_threads = 16,                           // OpenMP threads
    .bandwidth_priority = false,                 // Balance BW/latency
    .async_transfer = true,                      // Async halo exchange
    .workload_config = NULL
};

// Graph analytics: Irregular access, frontier-driven
static const pgas_tuning_t TUNING_GRAPH = {
    .memory_affinity = PGAS_AFFINITY_LOCAL,
    .partition_scheme = PGAS_PARTITION_BLOCK,
    .cache_line_align = true,
    .numa_bind = true,
    .batch_size = 128,
    .transfer_size = 512,                        // Small vertex data
    .prefetch_mode = PGAS_PREFETCH_NONE,         // Unpredictable
    .consistency = PGAS_CONSISTENCY_RELAXED,
    .num_threads = 4,
    .bandwidth_priority = false,
    .async_transfer = false,
    .workload_config = NULL
};

const pgas_tuning_t* pgas_get_default_tuning(pgas_profile_t profile) {
    switch (profile) {
        case PGAS_PROFILE_MCF:     return &TUNING_MCF;
        case PGAS_PROFILE_LLAMA:   return &TUNING_LLAMA;
        case PGAS_PROFILE_GROMACS: return &TUNING_GROMACS;
        case PGAS_PROFILE_GRAPH:   return &TUNING_GRAPH;
        case PGAS_PROFILE_DEFAULT:
        default:                   return &TUNING_DEFAULT;
    }
}

int pgas_load_profile(pgas_context_t* ctx, pgas_profile_t profile) {
    const pgas_tuning_t* tuning = pgas_get_default_tuning(profile);
    return pgas_set_tuning(ctx, tuning);
}

int pgas_set_tuning(pgas_context_t* ctx, const pgas_tuning_t* tuning) {
    if (!ctx || !tuning) return -1;

    // Copy tuning configuration
    memcpy(&g_current_tuning, tuning, sizeof(pgas_tuning_t));
    g_tuning_initialized = true;

    // Apply tuning to runtime
    // Note: Some settings take effect immediately, others at next allocation

    // Log tuning info
    const char* affinity_names[] = {"LOCAL", "REMOTE", "INTERLEAVE", "REPLICATE"};
    const char* prefetch_names[] = {"NONE", "SEQUENTIAL", "STRIDED", "AGGRESSIVE", "NEIGHBOR_LIST"};

    printf("PGAS Tuning Applied:\n");
    printf("  Memory affinity: %s\n", affinity_names[tuning->memory_affinity]);
    printf("  Batch size: %zu\n", tuning->batch_size);
    printf("  Transfer size: %zu\n", tuning->transfer_size);
    printf("  Prefetch mode: %s\n", prefetch_names[tuning->prefetch_mode]);
    printf("  Threads: %d\n", tuning->num_threads);
    printf("  Bandwidth priority: %s\n", tuning->bandwidth_priority ? "yes" : "no");

    return 0;
}

int pgas_get_tuning(pgas_context_t* ctx, pgas_tuning_t* tuning) {
    if (!ctx || !tuning) return -1;

    if (g_tuning_initialized) {
        memcpy(tuning, &g_current_tuning, sizeof(pgas_tuning_t));
    } else {
        memcpy(tuning, &TUNING_DEFAULT, sizeof(pgas_tuning_t));
    }

    return 0;
}

// Internal functions
static int comm_init(pgas_context_t* ctx, uint16_t port) {
    comm_handle_t* comm = calloc(1, sizeof(comm_handle_t));
    if (!comm) return -1;

    // Create listening socket
    comm->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (comm->listen_fd < 0) {
        free(comm);
        return -1;
    }

    int opt = 1;
    setsockopt(comm->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(comm->listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(comm->listen_fd);
        free(comm);
        return -1;
    }

    listen(comm->listen_fd, 16);

    // Allocate peer file descriptors - for OUTGOING requests (we initiated)
    comm->peer_fds = calloc(ctx->num_nodes, sizeof(int));
    // Allocate peer recv file descriptors - for receiving RESPONSES on connections we initiated
    comm->peer_recv_fds = calloc(ctx->num_nodes, sizeof(int));
    // Per-peer recv locks
    comm->recv_locks = calloc(ctx->num_nodes, sizeof(pthread_mutex_t));
    for (int i = 0; i < ctx->num_nodes; i++) {
        comm->peer_fds[i] = -1;
        comm->peer_recv_fds[i] = -1;
        pthread_mutex_init(&comm->recv_locks[i], NULL);
    }

    pthread_mutex_init(&comm->send_lock, NULL);
    pthread_mutex_init(&comm->peer_lock, NULL);
    pthread_mutex_init(&comm->pending_lock, NULL);
    pthread_cond_init(&comm->pending_cond, NULL);

    ctx->comm_handle = comm;

    // Start listener thread
    pthread_create(&comm->listener_thread, NULL, comm_listener_thread, ctx);

    // Give the listener thread time to start accepting connections
    usleep(100000);  // 100ms

    return 0;
}

static void comm_finalize(pgas_context_t* ctx) {
    comm_handle_t* comm = (comm_handle_t*)ctx->comm_handle;
    if (!comm) return;

    // Close all connections
    for (int i = 0; i < ctx->num_nodes; i++) {
        if (comm->peer_fds[i] >= 0) {
            close(comm->peer_fds[i]);
        }
        if (comm->peer_recv_fds[i] >= 0) {
            close(comm->peer_recv_fds[i]);
        }
        pthread_mutex_destroy(&comm->recv_locks[i]);
    }

    close(comm->listen_fd);
    free((void*)comm->peer_fds);
    free((void*)comm->peer_recv_fds);
    free(comm->recv_locks);

    pthread_mutex_destroy(&comm->send_lock);
    pthread_mutex_destroy(&comm->peer_lock);
    pthread_mutex_destroy(&comm->pending_lock);
    pthread_cond_destroy(&comm->pending_cond);

    free(comm);
    ctx->comm_handle = NULL;
}

static int comm_connect_peers(pgas_context_t* ctx) {
    comm_handle_t* comm = (comm_handle_t*)ctx->comm_handle;
    int max_retries = 30;  /* Wait up to 30 seconds for peers */
    int retry_delay_ms = 1000;

    /*
     * Connection protocol:
     * - Each node initiates connections to ALL other nodes
     * - The initiated connection is used for: sending requests, receiving responses
     * - The accepted connection (from peer) is used for: receiving requests, sending responses (handler thread)
     *
     * This ensures each pair has TWO sockets:
     * - Socket A: Node X initiated → Node Y (X sends requests, receives responses)
     * - Socket B: Node Y initiated → Node X (Y sends requests, receives responses)
     */

    int my_id = ctx->local_node_id;

    for (int retry = 0; retry < max_retries; retry++) {
        int connected = 0;
        int needed = 0;

        for (int i = 0; i < ctx->num_nodes; i++) {
            if (i == my_id) continue;
            if (!ctx->nodes[i].is_active) continue;

            needed++;

            /* Check if already connected (thread-safe read) */
            pthread_mutex_lock(&comm->peer_lock);
            int current_fd = comm->peer_fds[i];
            pthread_mutex_unlock(&comm->peer_lock);

            if (current_fd >= 0) {
                connected++;
                continue;
            }

            /* Initiate connection to ALL other nodes */
            int fd = socket(AF_INET, SOCK_STREAM, 0);
            if (fd < 0) continue;

            /* Set socket timeout */
            struct timeval tv;
            tv.tv_sec = 1;
            tv.tv_usec = 0;
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

            struct sockaddr_in addr = {0};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = ctx->nodes[i].ip_addr;
            addr.sin_port = htons(ctx->nodes[i].port);

            if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
                /* Send our node ID so the peer knows who we are */
                uint32_t my_node_id = my_id;
                send(fd, &my_node_id, sizeof(my_node_id), 0);

                pthread_mutex_lock(&comm->peer_lock);
                /* Use this socket for sending requests AND receiving responses */
                comm->peer_fds[i] = fd;
                comm->peer_recv_fds[i] = fd;
                pthread_mutex_unlock(&comm->peer_lock);

                printf("  Connected to node %d (%s:%d)\n",
                       i, ctx->nodes[i].hostname, ctx->nodes[i].port);
                connected++;
            } else {
                close(fd);
            }
        }

        if (needed == 0 || connected >= needed) {
            printf("  All peers connected (%d/%d)\n", connected, needed);
            return 0;
        }

        if (retry < max_retries - 1) {
            printf("  Waiting for peers... (%d/%d connected, retry %d/%d)\n",
                   connected, needed, retry + 1, max_retries);
            usleep(retry_delay_ms * 1000);
        }
    }

    /* Count final connections */
    int connected = 0;
    pthread_mutex_lock(&comm->peer_lock);
    for (int i = 0; i < ctx->num_nodes; i++) {
        if (i != my_id && comm->peer_fds[i] >= 0) {
            connected++;
        }
    }
    pthread_mutex_unlock(&comm->peer_lock);

    printf("  Peer connection: %d nodes connected\n", connected);
    return (connected > 0) ? 0 : -1;
}

/*
 * Send a request and receive the response atomically.
 * This ensures request-response pairing on the same socket.
 */
static int comm_send_recv(pgas_context_t* ctx, uint16_t node_id,
                          void* req_data, size_t req_len,
                          void* resp_data, size_t resp_len) {
    comm_handle_t* comm = (comm_handle_t*)ctx->comm_handle;

    if (node_id >= ctx->num_nodes || comm->peer_fds[node_id] < 0) {
        return -1;
    }

    int fd = comm->peer_fds[node_id];

    /* Lock the entire send-recv operation for this peer */
    pthread_mutex_lock(&comm->recv_locks[node_id]);

    /* Send the request */
    ssize_t sent = send(fd, req_data, req_len, 0);
    if (sent != (ssize_t)req_len) {
        pthread_mutex_unlock(&comm->recv_locks[node_id]);
        return -1;
    }

    /* Receive the response on the SAME socket */
    ssize_t received = recv(fd, resp_data, resp_len, MSG_WAITALL);

    pthread_mutex_unlock(&comm->recv_locks[node_id]);

    return (received > 0) ? (int)received : -1;
}

static int comm_send(pgas_context_t* ctx, uint16_t node_id, void* data, size_t len) {
    comm_handle_t* comm = (comm_handle_t*)ctx->comm_handle;

    if (node_id >= ctx->num_nodes || comm->peer_fds[node_id] < 0) {
        return -1;
    }

    pthread_mutex_lock(&comm->send_lock);
    ssize_t sent = send(comm->peer_fds[node_id], data, len, 0);
    pthread_mutex_unlock(&comm->send_lock);

    return (sent == (ssize_t)len) ? 0 : -1;
}

static int comm_recv(pgas_context_t* ctx, uint16_t node_id, void* data, size_t max_len) {
    comm_handle_t* comm = (comm_handle_t*)ctx->comm_handle;

    if (node_id >= ctx->num_nodes || comm->peer_recv_fds[node_id] < 0) {
        return -1;
    }

    /* Lock this peer's recv to avoid concurrent reads on same socket */
    pthread_mutex_lock(&comm->recv_locks[node_id]);
    ssize_t received = recv(comm->peer_recv_fds[node_id], data, max_len, MSG_WAITALL);
    pthread_mutex_unlock(&comm->recv_locks[node_id]);

    return (received > 0) ? (int)received : -1;
}

/* Thread arguments for connection handler */
typedef struct {
    pgas_context_t* ctx;
    int client_fd;
    int peer_node;
} conn_handler_args_t;

/* Handler thread for each incoming connection */
static void* conn_handler_thread(void* arg) {
    conn_handler_args_t* args = (conn_handler_args_t*)arg;
    pgas_context_t* ctx = args->ctx;
    int client_fd = args->client_fd;
    (void)args->peer_node;  // Unused for now
    free(args);

    // Handle incoming messages
    comm_message_t msg;
    while (1) {
        ssize_t r = recv(client_fd, &msg, sizeof(msg), 0);
        if (r <= 0) break;


            comm_message_t resp = {0};
            resp.header.src_node = ctx->local_node_id;
            resp.header.dst_node = msg.header.src_node;
            resp.header.request_id = msg.header.request_id;

            switch (msg.header.msg_type) {
                case MSG_GET: {
                    void* local_ptr = translate_address(ctx, msg.ptr);
                    if (local_ptr) {
                        resp.header.msg_type = MSG_GET_RESP;
                        // Send response with data
                        size_t resp_size = sizeof(comm_message_t) + msg.size;
                        comm_message_t* full_resp = malloc(resp_size);
                        *full_resp = resp;
                        memcpy(full_resp->data, local_ptr, msg.size);
                        send(client_fd, full_resp, resp_size, 0);
                        free(full_resp);
                    }
                    break;
                }

                case MSG_PUT: {
                    void* local_ptr = translate_address(ctx, msg.ptr);
                    if (local_ptr) {
                        // Read the data portion
                        char* data = malloc(msg.size);
                        recv(client_fd, data, msg.size, 0);
                        memcpy(local_ptr, data, msg.size);
                        cxl_flush(local_ptr, msg.size);
                        free(data);

                        resp.header.msg_type = MSG_PUT_RESP;
                        send(client_fd, &resp, sizeof(resp), 0);
                    }
                    break;
                }

                case MSG_ATOMIC_FAA: {
                    uint64_t* local_ptr = (uint64_t*)translate_address(ctx, msg.ptr);
                    if (local_ptr) {
                        resp.value = __sync_fetch_and_add(local_ptr, msg.value);
                        resp.header.msg_type = MSG_ATOMIC_RESP;
                        send(client_fd, &resp, sizeof(resp), 0);
                    }
                    break;
                }

                case MSG_ATOMIC_CAS: {
                    uint64_t* local_ptr = (uint64_t*)translate_address(ctx, msg.ptr);
                    if (local_ptr) {
                        resp.value = __sync_val_compare_and_swap(local_ptr, msg.value, msg.size);
                        resp.header.msg_type = MSG_ATOMIC_RESP;
                        send(client_fd, &resp, sizeof(resp), 0);
                    }
                    break;
                }

                case MSG_BARRIER:
                    resp.header.msg_type = MSG_BARRIER_RESP;
                    send(client_fd, &resp, sizeof(resp), 0);
                    break;

                case MSG_ALLOC: {
                    void* ptr = cxl_alloc((cxl_handle_t*)ctx->cxl_handle, msg.size, PGAS_CACHE_LINE_SIZE);
                    resp.header.msg_type = MSG_ALLOC_RESP;
                    if (ptr) {
                        resp.ptr.node_id = ctx->local_node_id;
                        resp.ptr.segment_id = 0;
                        resp.ptr.offset = (uint64_t)ptr - ctx->nodes[ctx->local_node_id].cxl_base;
                    } else {
                        resp.ptr = pgas_null_ptr();
                    }
                    send(client_fd, &resp, sizeof(resp), 0);
                    break;
                }

                case MSG_FREE: {
                    void* local_ptr = translate_address(ctx, msg.ptr);
                    if (local_ptr) {
                        cxl_free((cxl_handle_t*)ctx->cxl_handle, local_ptr);
                    }
                    break;
                }
            }
    }

    close(client_fd);
    return NULL;
}

/* Main listener thread - accepts connections and spawns handler threads */
static void* comm_listener_thread(void* arg) {
    pgas_context_t* ctx = (pgas_context_t*)arg;
    comm_handle_t* comm = (comm_handle_t*)ctx->comm_handle;

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);

        int client_fd = accept(comm->listen_fd, (struct sockaddr*)&client_addr, &addr_len);
        if (client_fd < 0) break;

        /* Read the node ID sent by the connecting peer */
        uint32_t peer_node_id = 0;
        ssize_t received = recv(client_fd, &peer_node_id, sizeof(peer_node_id), 0);

        int peer_node = -1;
        if (received == sizeof(peer_node_id) && peer_node_id < (uint32_t)ctx->num_nodes) {
            peer_node = (int)peer_node_id;
        }

        /* Fallback: try to identify by IP if node ID wasn't received */
        if (peer_node < 0) {
            for (int i = 0; i < ctx->num_nodes; i++) {
                if (i != ctx->local_node_id &&
                    ctx->nodes[i].ip_addr == client_addr.sin_addr.s_addr) {
                    peer_node = i;
                    break;
                }
            }
        }

        /* Last resort: use first available slot for localhost */
        if (peer_node < 0) {
            pthread_mutex_lock(&comm->peer_lock);
            for (int i = 0; i < ctx->num_nodes; i++) {
                if (i != ctx->local_node_id && comm->peer_fds[i] < 0) {
                    peer_node = i;
                    break;
                }
            }
            pthread_mutex_unlock(&comm->peer_lock);
        }

        if (peer_node >= 0 && peer_node != ctx->local_node_id) {
            /*
             * This is the connection that the peer INITIATED to us.
             * It will be used for RECEIVING requests from the peer.
             * Do NOT store in peer_fds - we use peer_fds for OUR initiated connections.
             */
            printf("  Accepted connection from node %d (fd=%d)\n", peer_node, client_fd);

            /* Spawn handler thread for this connection */
            conn_handler_args_t* args = malloc(sizeof(conn_handler_args_t));
            args->ctx = ctx;
            args->client_fd = client_fd;
            args->peer_node = peer_node;

            pthread_t handler;
            pthread_create(&handler, NULL, conn_handler_thread, args);
            pthread_detach(handler);
        } else {
            printf("  Unknown connection (peer_node=%d), closing\n", peer_node);
            close(client_fd);
        }
    }

    return NULL;
}

static int init_segments(pgas_context_t* ctx) {
    ctx->num_segments = ctx->num_nodes;
    ctx->segments = calloc(ctx->num_segments, sizeof(pgas_segment_t));
    if (!ctx->segments) return -1;

    for (int i = 0; i < ctx->num_nodes; i++) {
        ctx->segments[i].base_addr = ctx->nodes[i].cxl_base;
        ctx->segments[i].cxl_addr = ctx->nodes[i].cxl_base;
        ctx->segments[i].size = ctx->nodes[i].cxl_size;
        ctx->segments[i].owner_node = i;
        ctx->segments[i].is_mapped = (i == ctx->local_node_id);
        ctx->segments[i].is_shared = true;
    }

    return 0;
}

static void* translate_address(pgas_context_t* ctx, pgas_ptr_t ptr) {
    if (ptr.node_id != ctx->local_node_id) {
        return NULL;  // Not a local pointer
    }

    return (void*)(ctx->nodes[ptr.node_id].cxl_base + ptr.offset);
}
