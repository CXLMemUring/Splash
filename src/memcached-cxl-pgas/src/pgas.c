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
    int* peer_fds;
    pthread_t listener_thread;
    pthread_mutex_t send_lock;
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
        // Remote get
        comm_message_t* msg = malloc(sizeof(comm_message_t));
        if (!msg) return -1;

        msg->header.msg_type = MSG_GET;
        msg->header.src_node = ctx->local_node_id;
        msg->header.dst_node = src.node_id;
        msg->ptr = src;
        msg->size = size;

        comm_handle_t* comm = (comm_handle_t*)ctx->comm_handle;
        msg->header.request_id = __sync_fetch_and_add(&comm->next_request_id, 1);

        comm_send(ctx, src.node_id, msg, sizeof(*msg));

        // Wait for response
        comm_message_t* resp = malloc(sizeof(comm_message_t) + size);
        if (!resp) {
            free(msg);
            return -1;
        }

        comm_recv(ctx, src.node_id, resp, sizeof(comm_message_t) + size);

        if (resp->header.msg_type == MSG_GET_RESP) {
            memcpy(dest, resp->data, size);
            stats->remote_reads++;
            stats->bytes_transferred += size;
        }

        free(msg);
        free(resp);
    }

    return 0;
}

int pgas_put(pgas_context_t* ctx, pgas_ptr_t dest, const void* src, size_t size) {
    internal_stats_t* stats = get_stats(ctx);

    if (pgas_is_local(ctx, dest)) {
        // Local put
        void* local_ptr = translate_address(ctx, dest);
        if (!local_ptr) return -1;

        memcpy(local_ptr, src, size);
        cxl_flush(local_ptr, size);
        stats->local_writes++;
    } else {
        // Remote put
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

        comm_send(ctx, dest.node_id, msg, sizeof(comm_message_t) + size);

        // Wait for acknowledgment
        comm_message_t resp;
        comm_recv(ctx, dest.node_id, &resp, sizeof(resp));

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
        // Remote atomic
        comm_message_t msg = {0};
        msg.header.msg_type = MSG_ATOMIC_FAA;
        msg.header.src_node = ctx->local_node_id;
        msg.header.dst_node = ptr.node_id;
        msg.ptr = ptr;
        msg.value = value;

        comm_handle_t* comm = (comm_handle_t*)ctx->comm_handle;
        msg.header.request_id = __sync_fetch_and_add(&comm->next_request_id, 1);

        comm_send(ctx, ptr.node_id, &msg, sizeof(msg));

        comm_message_t resp;
        comm_recv(ctx, ptr.node_id, &resp, sizeof(resp));

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
        // Remote CAS
        comm_message_t msg = {0};
        msg.header.msg_type = MSG_ATOMIC_CAS;
        msg.header.src_node = ctx->local_node_id;
        msg.header.dst_node = ptr.node_id;
        msg.ptr = ptr;
        msg.value = expected;
        msg.size = desired;  // Reuse size field for desired value

        comm_handle_t* comm = (comm_handle_t*)ctx->comm_handle;
        msg.header.request_id = __sync_fetch_and_add(&comm->next_request_id, 1);

        comm_send(ctx, ptr.node_id, &msg, sizeof(msg));

        comm_message_t resp;
        comm_recv(ctx, ptr.node_id, &resp, sizeof(resp));

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

    // Allocate peer file descriptors
    comm->peer_fds = calloc(ctx->num_nodes, sizeof(int));
    for (int i = 0; i < ctx->num_nodes; i++) {
        comm->peer_fds[i] = -1;
    }

    pthread_mutex_init(&comm->send_lock, NULL);
    pthread_mutex_init(&comm->pending_lock, NULL);
    pthread_cond_init(&comm->pending_cond, NULL);

    ctx->comm_handle = comm;

    // Start listener thread
    pthread_create(&comm->listener_thread, NULL, comm_listener_thread, ctx);

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
    }

    close(comm->listen_fd);
    free(comm->peer_fds);

    pthread_mutex_destroy(&comm->send_lock);
    pthread_mutex_destroy(&comm->pending_lock);
    pthread_cond_destroy(&comm->pending_cond);

    free(comm);
    ctx->comm_handle = NULL;
}

static int comm_connect_peers(pgas_context_t* ctx) {
    comm_handle_t* comm = (comm_handle_t*)ctx->comm_handle;

    for (int i = 0; i < ctx->num_nodes; i++) {
        if (i == ctx->local_node_id) continue;
        if (!ctx->nodes[i].is_active) continue;

        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) continue;

        struct sockaddr_in addr = {0};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = ctx->nodes[i].ip_addr;
        addr.sin_port = htons(ctx->nodes[i].port);

        if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
            comm->peer_fds[i] = fd;
        } else {
            close(fd);
        }
    }

    return 0;
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

    if (node_id >= ctx->num_nodes || comm->peer_fds[node_id] < 0) {
        return -1;
    }

    ssize_t received = recv(comm->peer_fds[node_id], data, max_len, 0);
    return (received > 0) ? (int)received : -1;
}

static void* comm_listener_thread(void* arg) {
    pgas_context_t* ctx = (pgas_context_t*)arg;
    comm_handle_t* comm = (comm_handle_t*)ctx->comm_handle;

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);

        int client_fd = accept(comm->listen_fd, (struct sockaddr*)&client_addr, &addr_len);
        if (client_fd < 0) break;

        // Handle incoming messages
        comm_message_t msg;
        while (recv(client_fd, &msg, sizeof(msg), 0) > 0) {
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
