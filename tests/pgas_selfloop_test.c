/*
 * PGAS Self-Loop Test on CXL Memory (NUMA 2)
 * Tests the memcached-cxl-pgas PGAS abstraction layer
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <numa.h>
#include <numaif.h>

#include "../src/memcached-cxl-pgas/include/pgas.h"
#include "../src/memcached-cxl-pgas/include/cxl_memory.h"

#define CXL_NUMA_NODE 2
#define TEST_ITERATIONS 1000000
#define ARRAY_SIZE 10000

// Timer helper
static inline double get_time_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

// Self-loop write test: write value, read it back immediately
int test_selfloop_write_read(pgas_context_t* ctx, int iterations) {
    printf("\n=== Self-Loop Write/Read Test ===\n");

    // Allocate array on local CXL memory
    size_t array_bytes = ARRAY_SIZE * sizeof(uint64_t);
    pgas_ptr_t array_ptr = pgas_alloc(ctx, array_bytes, PGAS_AFFINITY_LOCAL);

    if (pgas_ptr_is_null(array_ptr)) {
        printf("  ERROR: Failed to allocate array\n");
        return -1;
    }

    printf("  Allocated %zu bytes on node %d\n", array_bytes, pgas_ptr_node(array_ptr));

    // Get local pointer for direct access
    uint64_t* local_array = (uint64_t*)pgas_local_ptr(ctx, array_ptr);
    if (!local_array) {
        printf("  ERROR: Failed to get local pointer\n");
        pgas_free(ctx, array_ptr);
        return -1;
    }

    // Initialize array
    for (int i = 0; i < ARRAY_SIZE; i++) {
        local_array[i] = 0;
    }
    pgas_fence(ctx, PGAS_CONSISTENCY_SEQ_CST);

    // Self-loop test: write value, read it back
    int errors = 0;
    double start = get_time_sec();

    for (int iter = 0; iter < iterations; iter++) {
        int idx = iter % ARRAY_SIZE;
        uint64_t value = iter + 1;

        // Write
        local_array[idx] = value;

        // Fence
        pgas_fence(ctx, PGAS_CONSISTENCY_SEQ_CST);

        // Read back (self-loop)
        uint64_t read_value = local_array[idx];

        // Verify
        if (read_value != value) {
            errors++;
            if (errors <= 5) {
                printf("  ERROR at iter %d: wrote %lu, read %lu\n",
                       iter, value, read_value);
            }
        }
    }

    double elapsed = get_time_sec() - start;
    double ops_per_sec = iterations / elapsed;

    printf("  Iterations: %d\n", iterations);
    printf("  Errors: %d\n", errors);
    printf("  Time: %.3f seconds\n", elapsed);
    printf("  Throughput: %.2f M ops/sec\n", ops_per_sec / 1e6);
    printf("  Avg latency: %.2f ns\n", elapsed * 1e9 / iterations);

    pgas_free(ctx, array_ptr);
    return errors;
}

// Self-loop PGAS put/get test
int test_selfloop_pgas_ops(pgas_context_t* ctx, int iterations) {
    printf("\n=== Self-Loop PGAS Put/Get Test ===\n");

    // Allocate single value
    pgas_ptr_t val_ptr = pgas_alloc(ctx, sizeof(uint64_t), PGAS_AFFINITY_LOCAL);

    if (pgas_ptr_is_null(val_ptr)) {
        printf("  ERROR: Failed to allocate value\n");
        return -1;
    }

    printf("  Allocated on node %d\n", pgas_ptr_node(val_ptr));

    int errors = 0;
    double start = get_time_sec();

    for (int iter = 0; iter < iterations; iter++) {
        uint64_t write_val = iter + 1;
        uint64_t read_val = 0;

        // Put (even though local, use PGAS API)
        pgas_put(ctx, val_ptr, &write_val, sizeof(uint64_t));

        // Fence
        pgas_fence(ctx, PGAS_CONSISTENCY_SEQ_CST);

        // Get (self-loop - same location)
        pgas_get(ctx, &read_val, val_ptr, sizeof(uint64_t));

        // Verify
        if (read_val != write_val) {
            errors++;
            if (errors <= 5) {
                printf("  ERROR at iter %d: put %lu, got %lu\n",
                       iter, write_val, read_val);
            }
        }
    }

    double elapsed = get_time_sec() - start;
    double ops_per_sec = iterations / elapsed;

    printf("  Iterations: %d\n", iterations);
    printf("  Errors: %d\n", errors);
    printf("  Time: %.3f seconds\n", elapsed);
    printf("  Throughput: %.2f K ops/sec\n", ops_per_sec / 1e3);
    printf("  Avg latency: %.2f us\n", elapsed * 1e6 / iterations);

    pgas_free(ctx, val_ptr);
    return errors;
}

// Self-loop atomic test
int test_selfloop_atomics(pgas_context_t* ctx, int iterations) {
    printf("\n=== Self-Loop Atomic Test ===\n");

    pgas_ptr_t counter_ptr = pgas_alloc(ctx, sizeof(uint64_t), PGAS_AFFINITY_LOCAL);

    if (pgas_ptr_is_null(counter_ptr)) {
        printf("  ERROR: Failed to allocate counter\n");
        return -1;
    }

    // Initialize to 0
    uint64_t zero = 0;
    pgas_put(ctx, counter_ptr, &zero, sizeof(uint64_t));
    pgas_fence(ctx, PGAS_CONSISTENCY_SEQ_CST);

    int errors = 0;
    double start = get_time_sec();

    for (int iter = 0; iter < iterations; iter++) {
        // Fetch-and-add
        uint64_t old_val = pgas_atomic_fetch_add(ctx, counter_ptr, 1);

        // Verify monotonic increase
        if (old_val != (uint64_t)iter) {
            errors++;
            if (errors <= 5) {
                printf("  ERROR at iter %d: expected %d, got %lu\n",
                       iter, iter, old_val);
            }
        }
    }

    double elapsed = get_time_sec() - start;
    double ops_per_sec = iterations / elapsed;

    // Verify final value
    uint64_t final_val;
    pgas_get(ctx, &final_val, counter_ptr, sizeof(uint64_t));

    printf("  Iterations: %d\n", iterations);
    printf("  Final counter value: %lu (expected: %d)\n", final_val, iterations);
    printf("  Errors: %d\n", errors);
    printf("  Time: %.3f seconds\n", elapsed);
    printf("  Throughput: %.2f K atomics/sec\n", ops_per_sec / 1e3);
    printf("  Avg latency: %.2f us\n", elapsed * 1e6 / iterations);

    if (final_val != (uint64_t)iterations) {
        errors++;
    }

    pgas_free(ctx, counter_ptr);
    return errors;
}

// Self-loop CAS test
int test_selfloop_cas(pgas_context_t* ctx, int iterations) {
    printf("\n=== Self-Loop CAS Test ===\n");

    pgas_ptr_t val_ptr = pgas_alloc(ctx, sizeof(uint64_t), PGAS_AFFINITY_LOCAL);

    if (pgas_ptr_is_null(val_ptr)) {
        printf("  ERROR: Failed to allocate value\n");
        return -1;
    }

    // Initialize to 0
    uint64_t zero = 0;
    pgas_put(ctx, val_ptr, &zero, sizeof(uint64_t));
    pgas_fence(ctx, PGAS_CONSISTENCY_SEQ_CST);

    int errors = 0;
    int cas_failures = 0;
    double start = get_time_sec();

    uint64_t current = 0;
    for (int iter = 0; iter < iterations; iter++) {
        uint64_t expected = current;
        uint64_t desired = current + 1;

        uint64_t old_val = pgas_atomic_cas(ctx, val_ptr, expected, desired);

        if (old_val == expected) {
            // CAS succeeded
            current = desired;
        } else {
            // CAS failed (shouldn't happen in single-threaded self-loop)
            cas_failures++;
            if (cas_failures <= 5) {
                printf("  CAS failed at iter %d: expected %lu, saw %lu\n",
                       iter, expected, old_val);
            }
            errors++;
        }
    }

    double elapsed = get_time_sec() - start;
    double ops_per_sec = iterations / elapsed;

    // Verify final value
    uint64_t final_val;
    pgas_get(ctx, &final_val, val_ptr, sizeof(uint64_t));

    printf("  Iterations: %d\n", iterations);
    printf("  Final value: %lu (expected: %d)\n", final_val, iterations);
    printf("  CAS failures: %d\n", cas_failures);
    printf("  Time: %.3f seconds\n", elapsed);
    printf("  Throughput: %.2f K CAS/sec\n", ops_per_sec / 1e3);
    printf("  Avg latency: %.2f us\n", elapsed * 1e6 / iterations);

    pgas_free(ctx, val_ptr);
    return errors;
}

// Memory bandwidth test using PGAS
int test_bandwidth(pgas_context_t* ctx, size_t size_mb) {
    printf("\n=== PGAS Bandwidth Test (%zu MB) ===\n", size_mb);

    size_t size = size_mb * 1024 * 1024;
    pgas_ptr_t buf_ptr = pgas_alloc(ctx, size, PGAS_AFFINITY_LOCAL);

    if (pgas_ptr_is_null(buf_ptr)) {
        printf("  ERROR: Failed to allocate buffer\n");
        return -1;
    }

    char* local_buf = (char*)pgas_local_ptr(ctx, buf_ptr);
    if (!local_buf) {
        printf("  ERROR: Failed to get local pointer\n");
        pgas_free(ctx, buf_ptr);
        return -1;
    }

    // Write test
    double start = get_time_sec();
    memset(local_buf, 0xAA, size);
    pgas_fence(ctx, PGAS_CONSISTENCY_SEQ_CST);
    double write_time = get_time_sec() - start;

    // Read test
    start = get_time_sec();
    volatile char sum = 0;
    for (size_t i = 0; i < size; i += 64) {
        sum += local_buf[i];
    }
    double read_time = get_time_sec() - start;

    printf("  Write bandwidth: %.2f GB/s\n", (size / 1e9) / write_time);
    printf("  Read bandwidth: %.2f GB/s\n", (size / 1e9) / read_time);

    (void)sum;  // Prevent optimization
    pgas_free(ctx, buf_ptr);
    return 0;
}

void print_usage(const char* prog) {
    printf("Usage: %s [options]\n\n", prog);
    printf("PGAS Self-Loop Test on CXL Memory\n\n");
    printf("Options:\n");
    printf("  -c, --config FILE   PGAS config file (will create default if not specified)\n");
    printf("  -i, --iterations N  Number of iterations (default: %d)\n", TEST_ITERATIONS);
    printf("  -m, --memory MB     Memory for bandwidth test (default: 64)\n");
    printf("  -h, --help          Show this help\n");
}

int main(int argc, char* argv[]) {
    const char* config_file = NULL;
    int iterations = 100000;  // Reduced for PGAS overhead
    size_t memory_mb = 64;

    // Parse arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--config") == 0) {
            config_file = argv[++i];
        } else if (strcmp(argv[i], "-i") == 0 || strcmp(argv[i], "--iterations") == 0) {
            iterations = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-m") == 0 || strcmp(argv[i], "--memory") == 0) {
            memory_mb = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
    }

    printf("╔════════════════════════════════════════════════════════════════╗\n");
    printf("║     PGAS Self-Loop Test on CXL Memory (NUMA %d)                ║\n", CXL_NUMA_NODE);
    printf("╚════════════════════════════════════════════════════════════════╝\n\n");

    // Create default config if not specified
    char default_config[] = "/tmp/pgas_selfloop_test.conf";
    if (!config_file) {
        FILE* f = fopen(default_config, "w");
        if (f) {
            fprintf(f, "# PGAS Self-Loop Test Configuration\n");
            fprintf(f, "local_node_id=0\n");
            fprintf(f, "num_nodes=1\n");
            fprintf(f, "node0=127.0.0.1:5000:0x0:1073741824\n");
            fclose(f);
            config_file = default_config;
            printf("Created default config: %s\n", config_file);
        }
    }

    printf("Configuration:\n");
    printf("  Config file: %s\n", config_file);
    printf("  Iterations: %d\n", iterations);
    printf("  Memory test size: %zu MB\n", memory_mb);
    printf("  Target NUMA node: %d\n", CXL_NUMA_NODE);
    printf("\n");

    // Set NUMA policy for CXL node
    if (numa_available() >= 0) {
        printf("Setting NUMA policy for node %d...\n", CXL_NUMA_NODE);
        struct bitmask* mask = numa_allocate_nodemask();
        numa_bitmask_setbit(mask, CXL_NUMA_NODE);
        numa_set_membind(mask);
        numa_free_nodemask(mask);
    }

    // Initialize PGAS
    printf("Initializing PGAS...\n");
    pgas_context_t ctx;
    if (pgas_init(&ctx, config_file) != 0) {
        printf("  Note: PGAS init may show warnings for single-node test\n");
        // Continue anyway for testing
    }

    printf("  Local node: %d\n", pgas_my_node(&ctx));
    printf("  Total nodes: %d\n", pgas_num_nodes(&ctx));
    printf("\n");

    int total_errors = 0;

    // Run tests
    total_errors += test_selfloop_write_read(&ctx, iterations);
    total_errors += test_selfloop_pgas_ops(&ctx, iterations / 10);  // Slower due to function call overhead
    total_errors += test_selfloop_atomics(&ctx, iterations / 10);
    total_errors += test_selfloop_cas(&ctx, iterations / 10);
    total_errors += test_bandwidth(&ctx, memory_mb);

    // Print PGAS stats
    printf("\n=== PGAS Statistics ===\n");
    pgas_stats_t stats;
    pgas_get_stats(&ctx, &stats);
    printf("  Local reads: %lu\n", stats.local_reads);
    printf("  Local writes: %lu\n", stats.local_writes);
    printf("  Remote reads: %lu\n", stats.remote_reads);
    printf("  Remote writes: %lu\n", stats.remote_writes);
    printf("  Atomics: %lu\n", stats.atomics);
    printf("  Avg latency: %.2f us\n", stats.avg_latency_us);

    // Cleanup
    pgas_finalize(&ctx);

    printf("\n=== Test Summary ===\n");
    if (total_errors == 0) {
        printf("  All tests PASSED ✓\n");
    } else {
        printf("  Tests FAILED with %d errors ✗\n", total_errors);
    }

    return total_errors > 0 ? 1 : 0;
}
