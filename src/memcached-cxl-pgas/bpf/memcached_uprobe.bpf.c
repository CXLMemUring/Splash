// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
// bpftime uprobe program for memcached interception
// This runs in userspace via bpftime runtime

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// BPF helpers (provided by bpftime)
#define SEC(name) __attribute__((section(name), used))

// Ring buffer for passing events to userspace handler
struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 256 * 1024);
} events SEC(".maps");

// Hash map for tracking in-flight requests
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 10000);
    __type(key, uint64_t);
    __type(value, struct mc_request_context);
} request_map SEC(".maps");

// Configuration map
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, uint32_t);
    __type(value, struct mc_bpf_config);
} config_map SEC(".maps");

// Memcached operation types
#define MC_OP_GET      0
#define MC_OP_SET      1
#define MC_OP_ADD      2
#define MC_OP_REPLACE  3
#define MC_OP_DELETE   4
#define MC_OP_INCR     5
#define MC_OP_DECR     6
#define MC_OP_APPEND   7
#define MC_OP_PREPEND  8
#define MC_OP_CAS      9

// Event types
#define EVENT_REQUEST_START  1
#define EVENT_REQUEST_END    2
#define EVENT_ITEM_GET       3
#define EVENT_ITEM_STORE     4
#define EVENT_ITEM_DELETE    5
#define EVENT_ITEM_ALLOC     6

// Maximum sizes
#define MAX_KEY_LEN    250
#define MAX_VALUE_LEN  1024  // Truncated for BPF

// BPF configuration
struct mc_bpf_config {
    bool enable_interception;
    bool enable_cxl_routing;
    bool enable_stats;
    uint16_t local_node_id;
    uint16_t num_nodes;
    uint64_t cxl_base_addr;
};

// Request context for tracking
struct mc_request_context {
    uint64_t start_time;
    uint8_t op_type;
    uint16_t key_len;
    char key[MAX_KEY_LEN];
    uint32_t value_len;
    uint64_t client_id;
    bool should_redirect;
    uint16_t target_node;
};

// Event structure passed to userspace
struct mc_event {
    uint8_t event_type;
    uint8_t op_type;
    uint64_t timestamp;
    uint64_t request_id;
    uint16_t key_len;
    char key[MAX_KEY_LEN];
    uint32_t value_len;
    uint32_t flags;
    uint32_t exptime;
    uint64_t cas_unique;
    uint16_t target_node;
    bool success;
    uint64_t latency_ns;
};

// Statistics
struct mc_stats {
    uint64_t total_requests;
    uint64_t get_requests;
    uint64_t set_requests;
    uint64_t delete_requests;
    uint64_t redirected_requests;
    uint64_t local_requests;
};

struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __uint(max_entries, 1);
    __type(key, uint32_t);
    __type(value, struct mc_stats);
} stats_map SEC(".maps");

// Helper to get current time in nanoseconds
static __always_inline uint64_t get_time_ns(void) {
    // bpftime provides this helper
    return bpf_ktime_get_ns();
}

// Helper to generate request ID
static __always_inline uint64_t gen_request_id(void) {
    return bpf_get_current_pid_tgid();
}

// Hash function for key routing (xxHash-like)
static __always_inline uint64_t hash_key(const char* key, uint16_t len) {
    uint64_t hash = 0x9e3779b97f4a7c15ULL;  // Golden ratio
    for (int i = 0; i < len && i < MAX_KEY_LEN; i++) {
        hash ^= ((uint64_t)key[i]) * 0x517cc1b727220a95ULL;
        hash = (hash << 17) | (hash >> 47);
    }
    return hash;
}

// Determine which node should handle this key
static __always_inline uint16_t route_to_node(uint64_t key_hash, uint16_t num_nodes) {
    return (uint16_t)(key_hash % num_nodes);
}

// Uprobe: intercept process_command (text protocol)
SEC("uprobe/memcached:process_command")
int uprobe_process_command(struct pt_regs *ctx) {
    uint32_t key = 0;
    struct mc_bpf_config *config = bpf_map_lookup_elem(&config_map, &key);
    if (!config || !config->enable_interception)
        return 0;

    // Extract command from registers (x86_64 calling convention)
    // void process_command(conn *c, char *command)
    char *command = (char *)ctx->si;  // Second argument

    // Parse command to extract operation type and key
    struct mc_event *event = bpf_ringbuf_reserve(&events, sizeof(*event), 0);
    if (!event)
        return 0;

    event->event_type = EVENT_REQUEST_START;
    event->timestamp = get_time_ns();
    event->request_id = gen_request_id();

    // Read command string (limited by BPF verifier)
    char cmd_buf[32];
    bpf_probe_read_user(cmd_buf, sizeof(cmd_buf), command);

    // Parse operation type
    if (cmd_buf[0] == 'g' && cmd_buf[1] == 'e' && cmd_buf[2] == 't') {
        event->op_type = MC_OP_GET;
    } else if (cmd_buf[0] == 's' && cmd_buf[1] == 'e' && cmd_buf[2] == 't') {
        event->op_type = MC_OP_SET;
    } else if (cmd_buf[0] == 'a' && cmd_buf[1] == 'd' && cmd_buf[2] == 'd') {
        event->op_type = MC_OP_ADD;
    } else if (cmd_buf[0] == 'd' && cmd_buf[1] == 'e' && cmd_buf[2] == 'l') {
        event->op_type = MC_OP_DELETE;
    } else if (cmd_buf[0] == 'i' && cmd_buf[1] == 'n' && cmd_buf[2] == 'c') {
        event->op_type = MC_OP_INCR;
    } else if (cmd_buf[0] == 'd' && cmd_buf[1] == 'e' && cmd_buf[2] == 'c') {
        event->op_type = MC_OP_DECR;
    } else if (cmd_buf[0] == 'c' && cmd_buf[1] == 'a' && cmd_buf[2] == 's') {
        event->op_type = MC_OP_CAS;
    } else {
        bpf_ringbuf_discard(event, 0);
        return 0;
    }

    // Extract key (after command and space)
    int key_start = 4;  // "get ", "set ", etc.
    event->key_len = 0;
    for (int i = 0; i < MAX_KEY_LEN && key_start + i < 32; i++) {
        char c = cmd_buf[key_start + i];
        if (c == ' ' || c == '\r' || c == '\n' || c == '\0')
            break;
        event->key[i] = c;
        event->key_len++;
    }

    // Determine routing
    if (config->enable_cxl_routing && config->num_nodes > 1) {
        uint64_t key_hash = hash_key(event->key, event->key_len);
        event->target_node = route_to_node(key_hash, config->num_nodes);
    } else {
        event->target_node = config->local_node_id;
    }

    // Update statistics
    struct mc_stats *stats = bpf_map_lookup_elem(&stats_map, &key);
    if (stats) {
        __sync_fetch_and_add(&stats->total_requests, 1);
        if (event->op_type == MC_OP_GET) {
            __sync_fetch_and_add(&stats->get_requests, 1);
        } else if (event->op_type == MC_OP_SET) {
            __sync_fetch_and_add(&stats->set_requests, 1);
        } else if (event->op_type == MC_OP_DELETE) {
            __sync_fetch_and_add(&stats->delete_requests, 1);
        }
        if (event->target_node != config->local_node_id) {
            __sync_fetch_and_add(&stats->redirected_requests, 1);
        } else {
            __sync_fetch_and_add(&stats->local_requests, 1);
        }
    }

    // Store context for return probe
    struct mc_request_context req_ctx = {
        .start_time = event->timestamp,
        .op_type = event->op_type,
        .key_len = event->key_len,
        .should_redirect = (event->target_node != config->local_node_id),
        .target_node = event->target_node,
    };
    for (int i = 0; i < event->key_len && i < MAX_KEY_LEN; i++) {
        req_ctx.key[i] = event->key[i];
    }

    uint64_t req_id = event->request_id;
    bpf_map_update_elem(&request_map, &req_id, &req_ctx, BPF_ANY);

    bpf_ringbuf_submit(event, 0);
    return 0;
}

// Uprobe: intercept item_get
SEC("uprobe/memcached:item_get")
int uprobe_item_get(struct pt_regs *ctx) {
    uint32_t key = 0;
    struct mc_bpf_config *config = bpf_map_lookup_elem(&config_map, &key);
    if (!config || !config->enable_interception)
        return 0;

    // item *item_get(const char *key, const size_t nkey, ...)
    char *item_key = (char *)ctx->di;    // First argument
    size_t nkey = (size_t)ctx->si;       // Second argument

    struct mc_event *event = bpf_ringbuf_reserve(&events, sizeof(*event), 0);
    if (!event)
        return 0;

    event->event_type = EVENT_ITEM_GET;
    event->op_type = MC_OP_GET;
    event->timestamp = get_time_ns();
    event->request_id = gen_request_id();

    // Copy key
    event->key_len = nkey < MAX_KEY_LEN ? nkey : MAX_KEY_LEN;
    bpf_probe_read_user(event->key, event->key_len, item_key);

    // Route decision
    if (config->enable_cxl_routing) {
        uint64_t key_hash = hash_key(event->key, event->key_len);
        event->target_node = route_to_node(key_hash, config->num_nodes);
    } else {
        event->target_node = config->local_node_id;
    }

    bpf_ringbuf_submit(event, 0);
    return 0;
}

// Uprobe: intercept do_item_alloc
SEC("uprobe/memcached:do_item_alloc")
int uprobe_do_item_alloc(struct pt_regs *ctx) {
    uint32_t key = 0;
    struct mc_bpf_config *config = bpf_map_lookup_elem(&config_map, &key);
    if (!config || !config->enable_interception)
        return 0;

    // item *do_item_alloc(char *key, const size_t nkey, ...)
    char *item_key = (char *)ctx->di;
    size_t nkey = (size_t)ctx->si;

    struct mc_event *event = bpf_ringbuf_reserve(&events, sizeof(*event), 0);
    if (!event)
        return 0;

    event->event_type = EVENT_ITEM_ALLOC;
    event->timestamp = get_time_ns();
    event->request_id = gen_request_id();

    event->key_len = nkey < MAX_KEY_LEN ? nkey : MAX_KEY_LEN;
    bpf_probe_read_user(event->key, event->key_len, item_key);

    // For allocation, determine where to place in CXL memory
    if (config->enable_cxl_routing) {
        uint64_t key_hash = hash_key(event->key, event->key_len);
        event->target_node = route_to_node(key_hash, config->num_nodes);
    }

    bpf_ringbuf_submit(event, 0);
    return 0;
}

// Uprobe: intercept item_link
SEC("uprobe/memcached:item_link")
int uprobe_item_link(struct pt_regs *ctx) {
    uint32_t key = 0;
    struct mc_bpf_config *config = bpf_map_lookup_elem(&config_map, &key);
    if (!config || !config->enable_interception)
        return 0;

    struct mc_event *event = bpf_ringbuf_reserve(&events, sizeof(*event), 0);
    if (!event)
        return 0;

    event->event_type = EVENT_ITEM_STORE;
    event->timestamp = get_time_ns();
    event->request_id = gen_request_id();

    // The item structure is passed as first argument
    // We would need to know the memcached item structure layout
    // This is simplified for demonstration

    bpf_ringbuf_submit(event, 0);
    return 0;
}

// Uprobe: intercept item_unlink
SEC("uprobe/memcached:item_unlink")
int uprobe_item_unlink(struct pt_regs *ctx) {
    uint32_t key = 0;
    struct mc_bpf_config *config = bpf_map_lookup_elem(&config_map, &key);
    if (!config || !config->enable_interception)
        return 0;

    struct mc_event *event = bpf_ringbuf_reserve(&events, sizeof(*event), 0);
    if (!event)
        return 0;

    event->event_type = EVENT_ITEM_DELETE;
    event->timestamp = get_time_ns();
    event->request_id = gen_request_id();

    bpf_ringbuf_submit(event, 0);
    return 0;
}

// Uretprobe: capture return values
SEC("uretprobe/memcached:item_get")
int uretprobe_item_get(struct pt_regs *ctx) {
    uint32_t key = 0;
    struct mc_bpf_config *config = bpf_map_lookup_elem(&config_map, &key);
    if (!config || !config->enable_interception)
        return 0;

    // Return value in rax
    void *item = (void *)ctx->ax;

    struct mc_event *event = bpf_ringbuf_reserve(&events, sizeof(*event), 0);
    if (!event)
        return 0;

    event->event_type = EVENT_REQUEST_END;
    event->timestamp = get_time_ns();
    event->request_id = gen_request_id();
    event->success = (item != NULL);

    // Look up original request to calculate latency
    uint64_t req_id = event->request_id;
    struct mc_request_context *req_ctx = bpf_map_lookup_elem(&request_map, &req_id);
    if (req_ctx) {
        event->latency_ns = event->timestamp - req_ctx->start_time;
        event->op_type = req_ctx->op_type;
        event->key_len = req_ctx->key_len;
        for (int i = 0; i < req_ctx->key_len && i < MAX_KEY_LEN; i++) {
            event->key[i] = req_ctx->key[i];
        }
        bpf_map_delete_elem(&request_map, &req_id);
    }

    bpf_ringbuf_submit(event, 0);
    return 0;
}

char LICENSE[] SEC("license") = "Dual BSD/GPL";
