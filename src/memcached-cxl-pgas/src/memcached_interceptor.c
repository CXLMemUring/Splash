#include "memcached_interceptor.h"
#include "cxl_memory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

// Hash table parameters
#define HASH_SEED 0x9747b28c

// Internal hash entry
typedef struct hash_entry {
    uint64_t key_hash;
    pgas_ptr_t meta_ptr;
    struct hash_entry* next;
} hash_entry_t;

// Latency tracking
typedef struct {
    uint64_t* samples;
    size_t count;
    size_t capacity;
    pthread_mutex_t lock;
} latency_tracker_t;

static latency_tracker_t latency_tracker = {0};

// MurmurHash3 finalizer
static inline uint64_t murmur3_fmix64(uint64_t k) {
    k ^= k >> 33;
    k *= 0xff51afd7ed558ccdULL;
    k ^= k >> 33;
    k *= 0xc4ceb9fe1a85ec53ULL;
    k ^= k >> 33;
    return k;
}

uint64_t mc_hash_key(const char* key, size_t key_len) {
    uint64_t h = HASH_SEED;

    // Simple but effective hash
    for (size_t i = 0; i < key_len; i++) {
        h ^= ((uint64_t)key[i]) << ((i % 8) * 8);
        if (i % 8 == 7 || i == key_len - 1) {
            h = murmur3_fmix64(h);
        }
    }

    return h;
}

uint16_t mc_route_key_to_node(mc_interceptor_t* interceptor, const char* key, size_t key_len) {
    uint64_t hash = mc_hash_key(key, key_len);
    return (uint16_t)(hash % pgas_num_nodes(interceptor->pgas_ctx));
}

int mc_interceptor_init(mc_interceptor_t** interceptor, pgas_context_t* pgas_ctx,
                       const mc_interceptor_config_t* config) {
    *interceptor = calloc(1, sizeof(mc_interceptor_t));
    if (!*interceptor) return -1;

    (*interceptor)->pgas_ctx = pgas_ctx;
    (*interceptor)->config = *config;

    // Initialize hash table
    (*interceptor)->hash_table_size = config->hash_table_size;
    (*interceptor)->hash_table = calloc(config->hash_table_size, sizeof(hash_entry_t*));
    if (!(*interceptor)->hash_table) {
        free(*interceptor);
        return -1;
    }

    // Allocate local cache
    if (config->local_cache_size > 0) {
        (*interceptor)->local_cache = malloc(config->local_cache_size);
        if (!(*interceptor)->local_cache) {
            free((*interceptor)->hash_table);
            free(*interceptor);
            return -1;
        }
        (*interceptor)->local_cache_used = 0;
    }

    // Allocate distributed hash table on CXL memory
    size_t remote_ht_size = config->hash_table_size * sizeof(pgas_ptr_t);
    (*interceptor)->remote_hash_table = pgas_alloc(pgas_ctx, remote_ht_size, PGAS_AFFINITY_INTERLEAVE);

    if (pgas_ptr_is_null((*interceptor)->remote_hash_table)) {
        fprintf(stderr, "Warning: Could not allocate remote hash table\n");
    }

    // Initialize latency tracker
    latency_tracker.capacity = 10000;
    latency_tracker.samples = malloc(latency_tracker.capacity * sizeof(uint64_t));
    latency_tracker.count = 0;
    pthread_mutex_init(&latency_tracker.lock, NULL);

    printf("Memcached interceptor initialized:\n");
    printf("  Hash table size: %zu\n", config->hash_table_size);
    printf("  Local cache size: %zu MB\n", config->local_cache_size / (1024 * 1024));
    printf("  CXL disaggregation: %s\n", config->enable_cxl_disaggregation ? "enabled" : "disabled");
    printf("  Replication: %s (factor: %d)\n",
           config->enable_replication ? "enabled" : "disabled",
           config->replication_factor);

    return 0;
}

void mc_interceptor_finalize(mc_interceptor_t* interceptor) {
    if (!interceptor) return;

    // Detach BPF programs
    mc_interceptor_detach_uprobes(interceptor);

    // Free hash table
    for (size_t i = 0; i < interceptor->hash_table_size; i++) {
        hash_entry_t* entry = interceptor->hash_table[i];
        while (entry) {
            hash_entry_t* next = entry->next;
            free(entry);
            entry = next;
        }
    }
    free(interceptor->hash_table);

    // Free local cache
    if (interceptor->local_cache) {
        free(interceptor->local_cache);
    }

    // Free remote hash table
    if (!pgas_ptr_is_null(interceptor->remote_hash_table)) {
        pgas_free(interceptor->pgas_ctx, interceptor->remote_hash_table);
    }

    // Cleanup latency tracker
    if (latency_tracker.samples) {
        free(latency_tracker.samples);
    }
    pthread_mutex_destroy(&latency_tracker.lock);

    free(interceptor);
}

mc_route_t mc_determine_route(mc_interceptor_t* interceptor, const mc_request_t* req) {
    if (!interceptor->config.enable_cxl_disaggregation) {
        return MC_ROUTE_LOCAL;
    }

    uint16_t target_node = mc_route_key_to_node(interceptor, req->key, req->key_len);
    uint16_t local_node = pgas_my_node(interceptor->pgas_ctx);

    if (target_node == local_node) {
        return MC_ROUTE_CXL_LOCAL;
    }

    return MC_ROUTE_CXL_REMOTE;
}

int mc_handle_request(mc_interceptor_t* interceptor, mc_request_t* req, mc_response_t* resp) {
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    interceptor->requests_total++;
    memset(resp, 0, sizeof(*resp));

    mc_route_t route = mc_determine_route(interceptor, req);
    int result = 0;

    switch (req->op) {
        case MC_OP_GET:
        case MC_OP_GETS:
            result = mc_item_fetch(interceptor, req->key, req->key_len, resp);
            if (result == 0 && resp->value != NULL) {
                interceptor->cache_hits++;
            } else {
                interceptor->cache_misses++;
            }
            break;

        case MC_OP_SET:
        case MC_OP_ADD:
        case MC_OP_REPLACE:
        case MC_OP_APPEND:
        case MC_OP_PREPEND: {
            pgas_ptr_t item_ptr;
            result = mc_item_store(interceptor, req, &item_ptr);
            resp->success = (result == 0);
            break;
        }

        case MC_OP_DELETE:
            result = mc_item_delete(interceptor, req->key, req->key_len);
            resp->success = (result == 0);
            break;

        case MC_OP_INCR:
        case MC_OP_DECR: {
            uint64_t new_value;
            result = mc_item_incr_decr(interceptor, req->key, req->key_len,
                                       req->delta, req->op == MC_OP_INCR, &new_value);
            resp->success = (result == 0);
            resp->cas_unique = new_value;  // Return new value
            break;
        }

        case MC_OP_CAS:
            result = mc_item_cas(interceptor, req, req->cas_unique, resp);
            break;

        case MC_OP_TOUCH:
            result = mc_item_touch(interceptor, req->key, req->key_len, req->exptime);
            resp->success = (result == 0);
            break;

        default:
            resp->success = false;
            resp->error_msg = "Unknown operation";
            result = -1;
    }

    // Track routing stats
    if (route == MC_ROUTE_LOCAL || route == MC_ROUTE_CXL_LOCAL) {
        interceptor->requests_local++;
    } else {
        interceptor->requests_remote++;
    }

    // Track latency
    clock_gettime(CLOCK_MONOTONIC, &end);
    uint64_t latency_ns = (end.tv_sec - start.tv_sec) * 1000000000ULL +
                          (end.tv_nsec - start.tv_nsec);

    pthread_mutex_lock(&latency_tracker.lock);
    if (latency_tracker.count < latency_tracker.capacity) {
        latency_tracker.samples[latency_tracker.count++] = latency_ns;
    }
    pthread_mutex_unlock(&latency_tracker.lock);

    return result;
}

int mc_item_store(mc_interceptor_t* interceptor, const mc_request_t* req, pgas_ptr_t* item_ptr) {
    uint64_t key_hash = mc_hash_key(req->key, req->key_len);
    uint16_t target_node = mc_route_key_to_node(interceptor, req->key, req->key_len);

    // Allocate item metadata on target node
    pgas_ptr_t meta_ptr = pgas_alloc_on_node(interceptor->pgas_ctx,
                                              sizeof(mc_item_meta_t), target_node);
    if (pgas_ptr_is_null(meta_ptr)) {
        return -1;
    }

    // Allocate space for key + value data
    size_t data_size = req->key_len + req->value_len;
    pgas_ptr_t data_ptr = pgas_alloc_on_node(interceptor->pgas_ctx, data_size, target_node);
    if (pgas_ptr_is_null(data_ptr)) {
        pgas_free(interceptor->pgas_ctx, meta_ptr);
        return -1;
    }

    // Prepare metadata
    mc_item_meta_t meta = {
        .key_hash = key_hash,
        .key_len = req->key_len,
        .value_len = req->value_len,
        .flags = req->flags,
        .exptime = req->exptime,
        .cas_unique = __sync_fetch_and_add(&interceptor->requests_total, 0),
        .data_ptr = data_ptr,
        .owner_node = target_node,
        .is_locked = false,
        .last_access = time(NULL)
    };

    // Write metadata
    pgas_put(interceptor->pgas_ctx, meta_ptr, &meta, sizeof(meta));

    // Write key + value data
    char* data_buf = malloc(data_size);
    if (!data_buf) {
        pgas_free(interceptor->pgas_ctx, meta_ptr);
        pgas_free(interceptor->pgas_ctx, data_ptr);
        return -1;
    }

    memcpy(data_buf, req->key, req->key_len);
    memcpy(data_buf + req->key_len, req->value, req->value_len);
    pgas_put(interceptor->pgas_ctx, data_ptr, data_buf, data_size);
    free(data_buf);

    // Update local hash table
    size_t bucket = key_hash % interceptor->hash_table_size;
    hash_entry_t* new_entry = malloc(sizeof(hash_entry_t));
    if (!new_entry) {
        pgas_free(interceptor->pgas_ctx, meta_ptr);
        pgas_free(interceptor->pgas_ctx, data_ptr);
        return -1;
    }

    new_entry->key_hash = key_hash;
    new_entry->meta_ptr = meta_ptr;
    new_entry->next = interceptor->hash_table[bucket];
    interceptor->hash_table[bucket] = new_entry;

    *item_ptr = meta_ptr;
    interceptor->cxl_writes++;

    return 0;
}

int mc_item_fetch(mc_interceptor_t* interceptor, const char* key, size_t key_len, mc_response_t* resp) {
    uint64_t key_hash = mc_hash_key(key, key_len);

    // Look up in local hash table
    size_t bucket = key_hash % interceptor->hash_table_size;
    hash_entry_t* entry = interceptor->hash_table[bucket];

    while (entry) {
        if (entry->key_hash == key_hash) {
            // Found potential match, fetch metadata from CXL
            mc_item_meta_t meta;
            pgas_get(interceptor->pgas_ctx, &meta, entry->meta_ptr, sizeof(meta));

            // Verify key match
            if (meta.key_len == key_len && meta.key_hash == key_hash) {
                // Check expiration
                if (meta.exptime != 0 && meta.exptime < time(NULL)) {
                    // Item expired
                    return -1;
                }

                // Fetch actual data
                size_t data_size = meta.key_len + meta.value_len;
                char* data_buf = malloc(data_size);
                if (!data_buf) return -1;

                pgas_get(interceptor->pgas_ctx, data_buf, meta.data_ptr, data_size);

                // Verify key
                if (memcmp(data_buf, key, key_len) != 0) {
                    free(data_buf);
                    entry = entry->next;
                    continue;
                }

                // Return value
                resp->value_len = meta.value_len;
                resp->value = malloc(meta.value_len);
                if (!resp->value) {
                    free(data_buf);
                    return -1;
                }

                memcpy(resp->value, data_buf + key_len, meta.value_len);
                resp->flags = meta.flags;
                resp->cas_unique = meta.cas_unique;
                resp->success = true;

                free(data_buf);
                interceptor->cxl_reads++;
                return 0;
            }
        }
        entry = entry->next;
    }

    return -1;  // Not found
}

int mc_item_delete(mc_interceptor_t* interceptor, const char* key, size_t key_len) {
    uint64_t key_hash = mc_hash_key(key, key_len);
    size_t bucket = key_hash % interceptor->hash_table_size;

    hash_entry_t** prev_ptr = &interceptor->hash_table[bucket];
    hash_entry_t* entry = *prev_ptr;

    while (entry) {
        if (entry->key_hash == key_hash) {
            // Fetch metadata to get data pointer
            mc_item_meta_t meta;
            pgas_get(interceptor->pgas_ctx, &meta, entry->meta_ptr, sizeof(meta));

            // Free CXL memory
            pgas_free(interceptor->pgas_ctx, meta.data_ptr);
            pgas_free(interceptor->pgas_ctx, entry->meta_ptr);

            // Remove from hash table
            *prev_ptr = entry->next;
            free(entry);

            return 0;
        }

        prev_ptr = &entry->next;
        entry = entry->next;
    }

    return -1;  // Not found
}

int mc_item_touch(mc_interceptor_t* interceptor, const char* key, size_t key_len, uint32_t exptime) {
    uint64_t key_hash = mc_hash_key(key, key_len);
    size_t bucket = key_hash % interceptor->hash_table_size;

    hash_entry_t* entry = interceptor->hash_table[bucket];
    while (entry) {
        if (entry->key_hash == key_hash) {
            // Update expiration time
            mc_item_meta_t meta;
            pgas_get(interceptor->pgas_ctx, &meta, entry->meta_ptr, sizeof(meta));

            meta.exptime = exptime;
            meta.last_access = time(NULL);

            pgas_put(interceptor->pgas_ctx, entry->meta_ptr, &meta, sizeof(meta));
            return 0;
        }
        entry = entry->next;
    }

    return -1;
}

int mc_item_incr_decr(mc_interceptor_t* interceptor, const char* key, size_t key_len,
                      uint64_t delta, bool incr, uint64_t* new_value) {
    uint64_t key_hash = mc_hash_key(key, key_len);
    size_t bucket = key_hash % interceptor->hash_table_size;

    hash_entry_t* entry = interceptor->hash_table[bucket];
    while (entry) {
        if (entry->key_hash == key_hash) {
            mc_item_meta_t meta;
            pgas_get(interceptor->pgas_ctx, &meta, entry->meta_ptr, sizeof(meta));

            // Fetch current value
            char* data_buf = malloc(meta.key_len + meta.value_len);
            if (!data_buf) return -1;

            pgas_get(interceptor->pgas_ctx, data_buf, meta.data_ptr, meta.key_len + meta.value_len);

            // Parse numeric value
            uint64_t current = 0;
            char* value_str = data_buf + meta.key_len;
            for (size_t i = 0; i < meta.value_len; i++) {
                if (value_str[i] >= '0' && value_str[i] <= '9') {
                    current = current * 10 + (value_str[i] - '0');
                }
            }

            // Apply operation
            if (incr) {
                current += delta;
            } else {
                if (current >= delta) {
                    current -= delta;
                } else {
                    current = 0;
                }
            }

            // Format new value
            char new_str[32];
            int new_len = snprintf(new_str, sizeof(new_str), "%lu", current);

            // Update data
            memcpy(data_buf + meta.key_len, new_str, new_len);
            meta.value_len = new_len;

            pgas_put(interceptor->pgas_ctx, meta.data_ptr, data_buf, meta.key_len + new_len);
            pgas_put(interceptor->pgas_ctx, entry->meta_ptr, &meta, sizeof(meta));

            *new_value = current;
            free(data_buf);
            return 0;
        }
        entry = entry->next;
    }

    return -1;
}

int mc_item_cas(mc_interceptor_t* interceptor, const mc_request_t* req,
                uint64_t cas_unique, mc_response_t* resp) {
    uint64_t key_hash = mc_hash_key(req->key, req->key_len);
    size_t bucket = key_hash % interceptor->hash_table_size;

    hash_entry_t* entry = interceptor->hash_table[bucket];
    while (entry) {
        if (entry->key_hash == key_hash) {
            mc_item_meta_t meta;
            pgas_get(interceptor->pgas_ctx, &meta, entry->meta_ptr, sizeof(meta));

            // Check CAS
            if (meta.cas_unique != cas_unique) {
                resp->success = false;
                resp->error_msg = "EXISTS";
                return -1;
            }

            // Update value atomically
            pgas_ptr_t old_data_ptr = meta.data_ptr;

            // Allocate new data
            size_t data_size = req->key_len + req->value_len;
            pgas_ptr_t new_data_ptr = pgas_alloc_on_node(interceptor->pgas_ctx,
                                                         data_size, meta.owner_node);
            if (pgas_ptr_is_null(new_data_ptr)) {
                return -1;
            }

            // Write new data
            char* data_buf = malloc(data_size);
            memcpy(data_buf, req->key, req->key_len);
            memcpy(data_buf + req->key_len, req->value, req->value_len);
            pgas_put(interceptor->pgas_ctx, new_data_ptr, data_buf, data_size);
            free(data_buf);

            // Update metadata
            meta.value_len = req->value_len;
            meta.flags = req->flags;
            meta.cas_unique++;
            meta.data_ptr = new_data_ptr;

            pgas_put(interceptor->pgas_ctx, entry->meta_ptr, &meta, sizeof(meta));

            // Free old data
            pgas_free(interceptor->pgas_ctx, old_data_ptr);

            resp->success = true;
            resp->cas_unique = meta.cas_unique;
            return 0;
        }
        entry = entry->next;
    }

    resp->success = false;
    resp->error_msg = "NOT_FOUND";
    return -1;
}

static int compare_uint64(const void* a, const void* b) {
    uint64_t va = *(const uint64_t*)a;
    uint64_t vb = *(const uint64_t*)b;
    if (va < vb) return -1;
    if (va > vb) return 1;
    return 0;
}

void mc_interceptor_get_stats(mc_interceptor_t* interceptor, mc_interceptor_stats_t* stats) {
    memset(stats, 0, sizeof(*stats));

    stats->total_requests = interceptor->requests_total;
    stats->local_hits = interceptor->requests_local;
    stats->remote_hits = interceptor->requests_remote;
    stats->cache_hits = interceptor->cache_hits;
    stats->cache_misses = interceptor->cache_misses;
    stats->cxl_bytes_read = interceptor->cxl_reads * 64;  // Approximate
    stats->cxl_bytes_written = interceptor->cxl_writes * 64;

    // Calculate latency statistics
    pthread_mutex_lock(&latency_tracker.lock);
    if (latency_tracker.count > 0) {
        uint64_t total = 0;
        for (size_t i = 0; i < latency_tracker.count; i++) {
            total += latency_tracker.samples[i];
        }
        stats->avg_latency_us = (double)total / latency_tracker.count / 1000.0;

        // Calculate p99
        qsort(latency_tracker.samples, latency_tracker.count, sizeof(uint64_t), compare_uint64);
        size_t p99_idx = (size_t)(latency_tracker.count * 0.99);
        stats->p99_latency_us = latency_tracker.samples[p99_idx] / 1000.0;
    }
    pthread_mutex_unlock(&latency_tracker.lock);
}

void mc_interceptor_reset_stats(mc_interceptor_t* interceptor) {
    interceptor->requests_total = 0;
    interceptor->requests_local = 0;
    interceptor->requests_remote = 0;
    interceptor->cache_hits = 0;
    interceptor->cache_misses = 0;
    interceptor->cxl_reads = 0;
    interceptor->cxl_writes = 0;

    pthread_mutex_lock(&latency_tracker.lock);
    latency_tracker.count = 0;
    pthread_mutex_unlock(&latency_tracker.lock);
}

void mc_interceptor_print_stats(mc_interceptor_t* interceptor) {
    mc_interceptor_stats_t stats;
    mc_interceptor_get_stats(interceptor, &stats);

    printf("\n=== Memcached Interceptor Statistics ===\n");
    printf("Total requests: %lu\n", stats.total_requests);
    printf("Local hits: %lu (%.2f%%)\n",
           stats.local_hits,
           stats.total_requests > 0 ? 100.0 * stats.local_hits / stats.total_requests : 0);
    printf("Remote hits: %lu (%.2f%%)\n",
           stats.remote_hits,
           stats.total_requests > 0 ? 100.0 * stats.remote_hits / stats.total_requests : 0);
    printf("Cache hits: %lu, misses: %lu (%.2f%% hit rate)\n",
           stats.cache_hits, stats.cache_misses,
           (stats.cache_hits + stats.cache_misses) > 0 ?
           100.0 * stats.cache_hits / (stats.cache_hits + stats.cache_misses) : 0);
    printf("CXL reads: %lu bytes, writes: %lu bytes\n",
           stats.cxl_bytes_read, stats.cxl_bytes_written);
    printf("Avg latency: %.2f μs, P99: %.2f μs\n",
           stats.avg_latency_us, stats.p99_latency_us);
    printf("========================================\n\n");
}

const char* mc_op_to_string(mc_op_type_t op) {
    static const char* names[] = {
        "GET", "SET", "ADD", "REPLACE", "DELETE",
        "INCR", "DECR", "APPEND", "PREPEND", "CAS",
        "GETS", "TOUCH", "GAT", "FLUSH", "STATS"
    };

    if (op < sizeof(names) / sizeof(names[0])) {
        return names[op];
    }
    return "UNKNOWN";
}

const char* mc_route_to_string(mc_route_t route) {
    switch (route) {
        case MC_ROUTE_LOCAL: return "LOCAL";
        case MC_ROUTE_REMOTE: return "REMOTE";
        case MC_ROUTE_CXL_LOCAL: return "CXL_LOCAL";
        case MC_ROUTE_CXL_REMOTE: return "CXL_REMOTE";
        case MC_ROUTE_REPLICATE: return "REPLICATE";
        default: return "UNKNOWN";
    }
}

// BPF loading functions (stubs - would need actual bpftime integration)
int mc_interceptor_load_bpf(mc_interceptor_t* interceptor, const char* memcached_path) {
    (void)interceptor;
    (void)memcached_path;
    printf("Loading BPF programs for %s\n", memcached_path);
    // In real implementation, would load bpf/memcached_uprobe.bpf.c
    return 0;
}

int mc_interceptor_attach_uprobes(mc_interceptor_t* interceptor) {
    (void)interceptor;
    printf("Attaching uprobes to memcached\n");
    // In real implementation, would attach uprobes
    return 0;
}

int mc_interceptor_detach_uprobes(mc_interceptor_t* interceptor) {
    (void)interceptor;
    printf("Detaching uprobes from memcached\n");
    return 0;
}
