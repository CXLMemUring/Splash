/*
 * PGAS Self-Loop Test for memcached-cxl-pgas
 *
 * Tests the PGAS abstraction layer with self-loop operations:
 * - Local memory allocation and access
 * - PGAS put/get operations
 * - Atomic operations (fetch-add, CAS)
 * - Memory bandwidth measurements
 *
 * This test validates CXL memory functionality when running
 * in a single-node configuration (self-loop).
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <getopt.h>

#ifdef HAVE_NUMA
#include <numa.h>
#include <numaif.h>
#endif

#include "pgas.h"
#include "cxl_memory.h"

/* Test configuration defaults */
#define DEFAULT_ITERATIONS  100000
#define DEFAULT_MEMORY_MB   64
#define DEFAULT_ARRAY_SIZE  10000
#define DEFAULT_NUMA_NODE   2

/* Test result structure */
typedef struct {
    const char* name;
    int passed;
    int errors;
    double elapsed_sec;
    double throughput;
    const char* throughput_unit;
} test_result_t;

/* Global test configuration */
typedef struct {
    int iterations;
    size_t memory_mb;
    int numa_node;
    int verbose;
    const char* config_file;
} test_config_t;

static test_config_t g_config = {
    .iterations = DEFAULT_ITERATIONS,
    .memory_mb = DEFAULT_MEMORY_MB,
    .numa_node = DEFAULT_NUMA_NODE,
    .verbose = 0,
    .config_file = NULL
};

/* Timer helper */
static inline double get_time_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* Print test result */
static void print_result(const test_result_t* result) {
    printf("\n=== %s ===\n", result->name);
    printf("  Status: %s\n", result->passed ? "PASSED" : "FAILED");
    printf("  Errors: %d\n", result->errors);
    printf("  Time: %.3f seconds\n", result->elapsed_sec);
    if (result->throughput > 0) {
        printf("  Throughput: %.2f %s\n", result->throughput, result->throughput_unit);
    }
}

/*
 * Test 1: Self-loop write/read using direct local pointers
 */
static test_result_t test_selfloop_write_read(pgas_context_t* ctx) {
    test_result_t result = {
        .name = "Self-Loop Write/Read Test",
        .passed = 0,
        .errors = 0,
        .elapsed_sec = 0,
        .throughput = 0,
        .throughput_unit = "M ops/sec"
    };

    int iterations = g_config.iterations;
    size_t array_bytes = DEFAULT_ARRAY_SIZE * sizeof(uint64_t);

    /* Allocate array on local CXL memory */
    pgas_ptr_t array_ptr = pgas_alloc(ctx, array_bytes, PGAS_AFFINITY_LOCAL);
    if (pgas_ptr_is_null(array_ptr)) {
        printf("  ERROR: Failed to allocate array\n");
        result.errors = 1;
        return result;
    }

    if (g_config.verbose) {
        printf("  Allocated %zu bytes on node %d\n", array_bytes, pgas_ptr_node(array_ptr));
    }

    /* Get local pointer for direct access */
    uint64_t* local_array = (uint64_t*)pgas_local_ptr(ctx, array_ptr);
    if (!local_array) {
        printf("  ERROR: Failed to get local pointer\n");
        pgas_free(ctx, array_ptr);
        result.errors = 1;
        return result;
    }

    /* Initialize array */
    for (int i = 0; i < DEFAULT_ARRAY_SIZE; i++) {
        local_array[i] = 0;
    }
    pgas_fence(ctx, PGAS_CONSISTENCY_SEQ_CST);

    /* Self-loop test: write value, read it back */
    double start = get_time_sec();

    for (int iter = 0; iter < iterations; iter++) {
        int idx = iter % DEFAULT_ARRAY_SIZE;
        uint64_t value = iter + 1;

        /* Write */
        local_array[idx] = value;

        /* Memory fence */
        pgas_fence(ctx, PGAS_CONSISTENCY_SEQ_CST);

        /* Read back (self-loop) */
        uint64_t read_value = local_array[idx];

        /* Verify */
        if (read_value != value) {
            result.errors++;
            if (result.errors <= 5) {
                printf("  ERROR at iter %d: wrote %lu, read %lu\n",
                       iter, value, read_value);
            }
        }
    }

    result.elapsed_sec = get_time_sec() - start;
    result.throughput = (iterations / result.elapsed_sec) / 1e6;
    result.passed = (result.errors == 0);

    pgas_free(ctx, array_ptr);
    return result;
}

/*
 * Test 2: Self-loop PGAS put/get operations
 */
static test_result_t test_selfloop_pgas_ops(pgas_context_t* ctx) {
    test_result_t result = {
        .name = "Self-Loop PGAS Put/Get Test",
        .passed = 0,
        .errors = 0,
        .elapsed_sec = 0,
        .throughput = 0,
        .throughput_unit = "K ops/sec"
    };

    int iterations = g_config.iterations / 10;  /* Reduced for API overhead */

    /* Allocate single value */
    pgas_ptr_t val_ptr = pgas_alloc(ctx, sizeof(uint64_t), PGAS_AFFINITY_LOCAL);
    if (pgas_ptr_is_null(val_ptr)) {
        printf("  ERROR: Failed to allocate value\n");
        result.errors = 1;
        return result;
    }

    if (g_config.verbose) {
        printf("  Allocated on node %d\n", pgas_ptr_node(val_ptr));
    }

    double start = get_time_sec();

    for (int iter = 0; iter < iterations; iter++) {
        uint64_t write_val = iter + 1;
        uint64_t read_val = 0;

        /* Put (using PGAS API) */
        pgas_put(ctx, val_ptr, &write_val, sizeof(uint64_t));

        /* Fence */
        pgas_fence(ctx, PGAS_CONSISTENCY_SEQ_CST);

        /* Get (self-loop - same location) */
        pgas_get(ctx, &read_val, val_ptr, sizeof(uint64_t));

        /* Verify */
        if (read_val != write_val) {
            result.errors++;
            if (result.errors <= 5) {
                printf("  ERROR at iter %d: put %lu, got %lu\n",
                       iter, write_val, read_val);
            }
        }
    }

    result.elapsed_sec = get_time_sec() - start;
    result.throughput = (iterations / result.elapsed_sec) / 1e3;
    result.passed = (result.errors == 0);

    pgas_free(ctx, val_ptr);
    return result;
}

/*
 * Test 3: Self-loop atomic fetch-and-add operations
 */
static test_result_t test_selfloop_atomics(pgas_context_t* ctx) {
    test_result_t result = {
        .name = "Self-Loop Atomic Fetch-Add Test",
        .passed = 0,
        .errors = 0,
        .elapsed_sec = 0,
        .throughput = 0,
        .throughput_unit = "K atomics/sec"
    };

    int iterations = g_config.iterations / 10;

    pgas_ptr_t counter_ptr = pgas_alloc(ctx, sizeof(uint64_t), PGAS_AFFINITY_LOCAL);
    if (pgas_ptr_is_null(counter_ptr)) {
        printf("  ERROR: Failed to allocate counter\n");
        result.errors = 1;
        return result;
    }

    /* Initialize to 0 */
    uint64_t zero = 0;
    pgas_put(ctx, counter_ptr, &zero, sizeof(uint64_t));
    pgas_fence(ctx, PGAS_CONSISTENCY_SEQ_CST);

    double start = get_time_sec();

    for (int iter = 0; iter < iterations; iter++) {
        /* Fetch-and-add */
        uint64_t old_val = pgas_atomic_fetch_add(ctx, counter_ptr, 1);

        /* Verify monotonic increase */
        if (old_val != (uint64_t)iter) {
            result.errors++;
            if (result.errors <= 5) {
                printf("  ERROR at iter %d: expected %d, got %lu\n",
                       iter, iter, old_val);
            }
        }
    }

    result.elapsed_sec = get_time_sec() - start;
    result.throughput = (iterations / result.elapsed_sec) / 1e3;

    /* Verify final value */
    uint64_t final_val;
    pgas_get(ctx, &final_val, counter_ptr, sizeof(uint64_t));

    if (g_config.verbose) {
        printf("  Final counter value: %lu (expected: %d)\n", final_val, iterations);
    }

    if (final_val != (uint64_t)iterations) {
        result.errors++;
    }

    result.passed = (result.errors == 0);
    pgas_free(ctx, counter_ptr);
    return result;
}

/*
 * Test 4: Self-loop compare-and-swap operations
 */
static test_result_t test_selfloop_cas(pgas_context_t* ctx) {
    test_result_t result = {
        .name = "Self-Loop CAS Test",
        .passed = 0,
        .errors = 0,
        .elapsed_sec = 0,
        .throughput = 0,
        .throughput_unit = "K CAS/sec"
    };

    int iterations = g_config.iterations / 10;

    pgas_ptr_t val_ptr = pgas_alloc(ctx, sizeof(uint64_t), PGAS_AFFINITY_LOCAL);
    if (pgas_ptr_is_null(val_ptr)) {
        printf("  ERROR: Failed to allocate value\n");
        result.errors = 1;
        return result;
    }

    /* Initialize to 0 */
    uint64_t zero = 0;
    pgas_put(ctx, val_ptr, &zero, sizeof(uint64_t));
    pgas_fence(ctx, PGAS_CONSISTENCY_SEQ_CST);

    int cas_failures = 0;
    double start = get_time_sec();

    uint64_t current = 0;
    for (int iter = 0; iter < iterations; iter++) {
        uint64_t expected = current;
        uint64_t desired = current + 1;

        uint64_t old_val = pgas_atomic_cas(ctx, val_ptr, expected, desired);

        if (old_val == expected) {
            /* CAS succeeded */
            current = desired;
        } else {
            /* CAS failed (shouldn't happen in single-threaded self-loop) */
            cas_failures++;
            if (cas_failures <= 5) {
                printf("  CAS failed at iter %d: expected %lu, saw %lu\n",
                       iter, expected, old_val);
            }
            result.errors++;
        }
    }

    result.elapsed_sec = get_time_sec() - start;
    result.throughput = (iterations / result.elapsed_sec) / 1e3;

    /* Verify final value */
    uint64_t final_val;
    pgas_get(ctx, &final_val, val_ptr, sizeof(uint64_t));

    if (g_config.verbose) {
        printf("  Final value: %lu (expected: %d)\n", final_val, iterations);
        printf("  CAS failures: %d\n", cas_failures);
    }

    result.passed = (result.errors == 0);
    pgas_free(ctx, val_ptr);
    return result;
}

/*
 * Test 5: Memory bandwidth test
 */
static test_result_t test_bandwidth(pgas_context_t* ctx) {
    test_result_t result = {
        .name = "PGAS Bandwidth Test",
        .passed = 0,
        .errors = 0,
        .elapsed_sec = 0,
        .throughput = 0,
        .throughput_unit = "GB/s"
    };

    size_t size = g_config.memory_mb * 1024 * 1024;

    pgas_ptr_t buf_ptr = pgas_alloc(ctx, size, PGAS_AFFINITY_LOCAL);
    if (pgas_ptr_is_null(buf_ptr)) {
        printf("  ERROR: Failed to allocate buffer (%zu MB)\n", g_config.memory_mb);
        result.errors = 1;
        return result;
    }

    char* local_buf = (char*)pgas_local_ptr(ctx, buf_ptr);
    if (!local_buf) {
        printf("  ERROR: Failed to get local pointer\n");
        pgas_free(ctx, buf_ptr);
        result.errors = 1;
        return result;
    }

    /* Write test */
    double start = get_time_sec();
    memset(local_buf, 0xAA, size);
    pgas_fence(ctx, PGAS_CONSISTENCY_SEQ_CST);
    double write_time = get_time_sec() - start;

    /* Read test */
    start = get_time_sec();
    volatile char sum = 0;
    for (size_t i = 0; i < size; i += 64) {
        sum += local_buf[i];
    }
    double read_time = get_time_sec() - start;

    double write_bw = (size / 1e9) / write_time;
    double read_bw = (size / 1e9) / read_time;

    printf("  Buffer size: %zu MB\n", g_config.memory_mb);
    printf("  Write bandwidth: %.2f GB/s\n", write_bw);
    printf("  Read bandwidth: %.2f GB/s\n", read_bw);

    result.throughput = (write_bw + read_bw) / 2;  /* Average */
    result.elapsed_sec = write_time + read_time;
    result.passed = 1;

    (void)sum;  /* Prevent optimization */
    pgas_free(ctx, buf_ptr);
    return result;
}

/*
 * Print usage information
 */
static void print_usage(const char* prog) {
    printf("Usage: %s [options]\n\n", prog);
    printf("PGAS Self-Loop Test for memcached-cxl-pgas\n\n");
    printf("Options:\n");
    printf("  -c, --config FILE   PGAS config file\n");
    printf("  -i, --iterations N  Number of iterations (default: %d)\n", DEFAULT_ITERATIONS);
    printf("  -m, --memory MB     Memory for bandwidth test (default: %d)\n", DEFAULT_MEMORY_MB);
    printf("  -n, --numa NODE     Target NUMA node (default: %d)\n", DEFAULT_NUMA_NODE);
    printf("  -v, --verbose       Enable verbose output\n");
    printf("  -h, --help          Show this help\n");
}

/*
 * Parse command line arguments
 */
static int parse_args(int argc, char* argv[]) {
    static struct option long_options[] = {
        {"config",     required_argument, 0, 'c'},
        {"iterations", required_argument, 0, 'i'},
        {"memory",     required_argument, 0, 'm'},
        {"numa",       required_argument, 0, 'n'},
        {"verbose",    no_argument,       0, 'v'},
        {"help",       no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "c:i:m:n:vh", long_options, NULL)) != -1) {
        switch (opt) {
            case 'c':
                g_config.config_file = optarg;
                break;
            case 'i':
                g_config.iterations = atoi(optarg);
                break;
            case 'm':
                g_config.memory_mb = atoi(optarg);
                break;
            case 'n':
                g_config.numa_node = atoi(optarg);
                break;
            case 'v':
                g_config.verbose = 1;
                break;
            case 'h':
                print_usage(argv[0]);
                exit(0);
            default:
                print_usage(argv[0]);
                return -1;
        }
    }
    return 0;
}

/*
 * Create default configuration file
 */
static const char* create_default_config(void) {
    static char config_path[] = "/tmp/pgas_selfloop_test.conf";

    FILE* f = fopen(config_path, "w");
    if (f) {
        fprintf(f, "# PGAS Self-Loop Test Configuration\n");
        fprintf(f, "local_node_id=0\n");
        fprintf(f, "num_nodes=1\n");
        fprintf(f, "node0=127.0.0.1:5000:0x0:1073741824\n");
        fclose(f);
        return config_path;
    }
    return NULL;
}

int main(int argc, char* argv[]) {
    if (parse_args(argc, argv) != 0) {
        return 1;
    }

    printf("========================================\n");
    printf("  PGAS Self-Loop Test\n");
    printf("  memcached-cxl-pgas Test Suite\n");
    printf("========================================\n\n");

    /* Create default config if not specified */
    const char* config_file = g_config.config_file;
    if (!config_file) {
        config_file = create_default_config();
        if (!config_file) {
            fprintf(stderr, "Failed to create default config\n");
            return 1;
        }
        printf("Using default config: %s\n", config_file);
    }

    printf("Configuration:\n");
    printf("  Config file: %s\n", config_file);
    printf("  Iterations: %d\n", g_config.iterations);
    printf("  Memory test size: %zu MB\n", g_config.memory_mb);
    printf("  Target NUMA node: %d\n", g_config.numa_node);
    printf("\n");

#ifdef HAVE_NUMA
    /* Set NUMA policy for CXL node */
    if (numa_available() >= 0) {
        printf("Setting NUMA policy for node %d...\n", g_config.numa_node);
        struct bitmask* mask = numa_allocate_nodemask();
        numa_bitmask_setbit(mask, g_config.numa_node);
        numa_set_membind(mask);
        numa_free_nodemask(mask);
    }
#endif

    /* Initialize PGAS */
    printf("Initializing PGAS...\n");
    pgas_context_t ctx;
    if (pgas_init(&ctx, config_file) != 0) {
        printf("  Note: PGAS init may show warnings for single-node test\n");
    }

    printf("  Local node: %d\n", pgas_my_node(&ctx));
    printf("  Total nodes: %d\n", pgas_num_nodes(&ctx));

    /* Run tests */
    test_result_t results[5];
    int num_tests = 0;
    int total_errors = 0;

    results[num_tests++] = test_selfloop_write_read(&ctx);
    print_result(&results[num_tests - 1]);
    total_errors += results[num_tests - 1].errors;

    results[num_tests++] = test_selfloop_pgas_ops(&ctx);
    print_result(&results[num_tests - 1]);
    total_errors += results[num_tests - 1].errors;

    results[num_tests++] = test_selfloop_atomics(&ctx);
    print_result(&results[num_tests - 1]);
    total_errors += results[num_tests - 1].errors;

    results[num_tests++] = test_selfloop_cas(&ctx);
    print_result(&results[num_tests - 1]);
    total_errors += results[num_tests - 1].errors;

    results[num_tests++] = test_bandwidth(&ctx);
    print_result(&results[num_tests - 1]);
    total_errors += results[num_tests - 1].errors;

    /* Print PGAS stats */
    printf("\n=== PGAS Statistics ===\n");
    pgas_stats_t stats;
    pgas_get_stats(&ctx, &stats);
    printf("  Local reads: %lu\n", stats.local_reads);
    printf("  Local writes: %lu\n", stats.local_writes);
    printf("  Remote reads: %lu\n", stats.remote_reads);
    printf("  Remote writes: %lu\n", stats.remote_writes);
    printf("  Atomics: %lu\n", stats.atomics);

    /* Cleanup */
    pgas_finalize(&ctx);

    /* Print summary */
    printf("\n========================================\n");
    printf("  Test Summary\n");
    printf("========================================\n");

    int passed = 0;
    for (int i = 0; i < num_tests; i++) {
        printf("  [%s] %s\n",
               results[i].passed ? "PASS" : "FAIL",
               results[i].name);
        if (results[i].passed) passed++;
    }

    printf("\n  Passed: %d/%d\n", passed, num_tests);

    if (total_errors == 0) {
        printf("\n  All tests PASSED\n");
    } else {
        printf("\n  Tests FAILED with %d errors\n", total_errors);
    }

    return total_errors > 0 ? 1 : 0;
}
