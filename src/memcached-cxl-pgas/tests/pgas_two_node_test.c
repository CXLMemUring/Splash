/*
 * PGAS Two-Node Self-Loop Test
 *
 * Tests PGAS communication between two processes running on the same machine.
 * Each process acts as a separate PGAS node and communicates with the other.
 *
 * Usage:
 *   # Terminal 1 (Node 0):
 *   ./pgas_two_node_test -c config/node0.conf
 *
 *   # Terminal 2 (Node 1):
 *   ./pgas_two_node_test -c config/node1.conf
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <getopt.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <errno.h>

#ifdef HAVE_NUMA
#include <numa.h>
#include <numaif.h>
#endif

#include "pgas.h"
#include "cxl_memory.h"

/* Test configuration */
#define DEFAULT_ITERATIONS  10000
#define DEFAULT_MESSAGE_SIZE 64
#define SHARED_ARRAY_SIZE   1024
#define SYNC_TIMEOUT_SEC    30

/* Fixed offsets for each node's shared region (must match between nodes) */
#define NODE0_REGION_OFFSET  0x1000   /* 4KB offset for Node 0's data */
#define NODE1_REGION_OFFSET  0x10000  /* 64KB offset for Node 1's data */
#define REGION_SPACING       0x10000  /* 64KB between regions */

/* Synchronization states */
typedef enum {
    SYNC_INIT = 0,
    SYNC_READY = 1,
    SYNC_RUNNING = 2,
    SYNC_DONE = 3
} sync_state_t;

/* Shared region structure (at known offset in each node's memory) */
typedef struct {
    volatile uint64_t sync_state;
    volatile uint64_t counter;
    volatile uint64_t remote_ready;
    uint64_t data[SHARED_ARRAY_SIZE];
} shared_region_t;

/* Test result */
typedef struct {
    const char* name;
    int passed;
    int errors;
    double elapsed_sec;
    double throughput;
    const char* unit;
} test_result_t;

/* Global state */
static pgas_context_t g_ctx;
static volatile int g_running = 1;
static int g_node_id = -1;
static int g_peer_id = -1;
static int g_num_nodes = 0;
static pgas_ptr_t g_local_region;
static pgas_ptr_t g_peer_region;  /* Fixed offset to peer's region */
static shared_region_t* g_local_shared = NULL;

/* Timer helper */
static inline double get_time_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* Signal handler */
static void signal_handler(int sig) {
    (void)sig;
    g_running = 0;
}

/* Wait for peer to reach a certain state */
static int wait_for_peer_state(pgas_context_t* ctx, pgas_ptr_t peer_region,
                                uint64_t expected_state, int timeout_sec) {
    double start = get_time_sec();
    uint64_t state = 0;

    while (g_running) {
        pgas_get(ctx, &state, peer_region, sizeof(uint64_t));
        if (state >= expected_state) {
            return 0;
        }

        if (get_time_sec() - start > timeout_sec) {
            fprintf(stderr, "Timeout waiting for peer state %lu (current: %lu)\n",
                    expected_state, state);
            return -1;
        }

        usleep(10000);  /* 10ms */
    }
    return -1;
}

/* Set local state */
static void set_local_state(uint64_t state) {
    g_local_shared->sync_state = state;
    pgas_fence(&g_ctx, PGAS_CONSISTENCY_SEQ_CST);
}

/*
 * Test 1: Ping-pong latency test
 */
static test_result_t test_ping_pong(pgas_context_t* ctx, int iterations) {
    test_result_t result = {
        .name = "Ping-Pong Latency Test",
        .passed = 0,
        .errors = 0,
        .elapsed_sec = 0,
        .throughput = 0,
        .unit = "us/roundtrip"
    };

    printf("\n=== %s ===\n", result.name);
    printf("  Node %d: %s\n", g_node_id, g_node_id == 0 ? "Initiator" : "Responder");

    /* Create peer region pointer (using peer's known region offset) */
    pgas_ptr_t peer_counter = {
        .node_id = g_peer_id,
        .segment_id = 0,
        .offset = g_peer_region.offset + offsetof(shared_region_t, counter),
        .flags = 0
    };

    pgas_ptr_t peer_ready = {
        .node_id = g_peer_id,
        .segment_id = 0,
        .offset = g_peer_region.offset + offsetof(shared_region_t, remote_ready),
        .flags = 0
    };

    /* Initialize */
    g_local_shared->counter = 0;
    g_local_shared->remote_ready = 0;
    pgas_fence(ctx, PGAS_CONSISTENCY_SEQ_CST);

    /* Signal ready */
    g_local_shared->remote_ready = 1;
    pgas_fence(ctx, PGAS_CONSISTENCY_SEQ_CST);

    /* Wait for peer ready */
    printf("  Waiting for peer...\n");
    uint64_t peer_ready_val = 0;
    double wait_start = get_time_sec();
    while (peer_ready_val == 0 && g_running) {
        pgas_get(ctx, &peer_ready_val, peer_ready, sizeof(uint64_t));
        if (get_time_sec() - wait_start > SYNC_TIMEOUT_SEC) {
            printf("  Timeout waiting for peer\n");
            result.errors = 1;
            return result;
        }
        usleep(1000);
    }
    printf("  Peer ready, starting test...\n");

    double start = get_time_sec();

    if (g_node_id == 0) {
        /* Initiator: send ping, wait for pong */
        for (int i = 0; i < iterations && g_running; i++) {
            uint64_t ping_val = i + 1;
            uint64_t pong_val = 0;

            /* Send ping (write to peer) */
            pgas_put(ctx, peer_counter, &ping_val, sizeof(uint64_t));
            pgas_fence(ctx, PGAS_CONSISTENCY_SEQ_CST);

            /* Wait for pong (peer increments our counter) */
            while (pong_val < ping_val && g_running) {
                pong_val = g_local_shared->counter;
            }

            if (pong_val != ping_val) {
                result.errors++;
            }
        }
    } else {
        /* Responder: wait for ping, send pong */
        for (int i = 0; i < iterations && g_running; i++) {
            uint64_t expected = i + 1;

            /* Wait for ping */
            while (g_local_shared->counter < expected && g_running) {
                /* Spin wait */
            }

            /* Send pong (write to peer) */
            pgas_put(ctx, peer_counter, &expected, sizeof(uint64_t));
            pgas_fence(ctx, PGAS_CONSISTENCY_SEQ_CST);
        }
    }

    result.elapsed_sec = get_time_sec() - start;
    result.throughput = (result.elapsed_sec * 1e6) / iterations;  /* us per roundtrip */
    result.passed = (result.errors == 0);

    printf("  Iterations: %d\n", iterations);
    printf("  Errors: %d\n", result.errors);
    printf("  Total time: %.3f sec\n", result.elapsed_sec);
    printf("  Avg latency: %.2f us/roundtrip\n", result.throughput);

    return result;
}

/*
 * Test 2: Remote atomic increment test
 */
static test_result_t test_remote_atomics(pgas_context_t* ctx, int iterations) {
    test_result_t result = {
        .name = "Remote Atomic Increment Test",
        .passed = 0,
        .errors = 0,
        .elapsed_sec = 0,
        .throughput = 0,
        .unit = "K atomics/sec"
    };

    printf("\n=== %s ===\n", result.name);

    /* Reset counter */
    g_local_shared->counter = 0;
    g_local_shared->remote_ready = 0;
    pgas_fence(ctx, PGAS_CONSISTENCY_SEQ_CST);

    /* Create pointer to peer's counter (using peer's known offset) */
    pgas_ptr_t peer_counter = {
        .node_id = g_peer_id,
        .segment_id = 0,
        .offset = g_peer_region.offset + offsetof(shared_region_t, counter),
        .flags = 0
    };

    pgas_ptr_t peer_ready = {
        .node_id = g_peer_id,
        .segment_id = 0,
        .offset = g_peer_region.offset + offsetof(shared_region_t, remote_ready),
        .flags = 0
    };

    /* Signal ready */
    g_local_shared->remote_ready = 1;
    pgas_fence(ctx, PGAS_CONSISTENCY_SEQ_CST);

    /* Wait for peer */
    printf("  Waiting for peer...\n");
    uint64_t peer_ready_val = 0;
    double wait_start = get_time_sec();
    while (peer_ready_val == 0 && g_running) {
        pgas_get(ctx, &peer_ready_val, peer_ready, sizeof(uint64_t));
        if (get_time_sec() - wait_start > SYNC_TIMEOUT_SEC) {
            printf("  Timeout waiting for peer\n");
            result.errors = 1;
            return result;
        }
        usleep(1000);
    }
    printf("  Peer ready, starting test...\n");

    /* Both nodes increment peer's counter */
    double start = get_time_sec();

    for (int i = 0; i < iterations && g_running; i++) {
        pgas_atomic_fetch_add(ctx, peer_counter, 1);
    }

    pgas_fence(ctx, PGAS_CONSISTENCY_SEQ_CST);
    result.elapsed_sec = get_time_sec() - start;

    /* Wait a bit for peer to finish */
    usleep(100000);

    /* Check our local counter (peer should have incremented it) */
    uint64_t local_count = g_local_shared->counter;
    printf("  Local counter value: %lu (expected: %d from peer)\n", local_count, iterations);

    if (local_count != (uint64_t)iterations) {
        result.errors++;
    }

    result.throughput = (iterations / result.elapsed_sec) / 1e3;
    result.passed = (result.errors == 0);

    printf("  Iterations: %d\n", iterations);
    printf("  Errors: %d\n", result.errors);
    printf("  Time: %.3f sec\n", result.elapsed_sec);
    printf("  Throughput: %.2f K atomics/sec\n", result.throughput);

    return result;
}

/*
 * Test 3: Bulk data transfer test
 */
static test_result_t test_bulk_transfer(pgas_context_t* ctx, int iterations, size_t msg_size) {
    test_result_t result = {
        .name = "Bulk Data Transfer Test",
        .passed = 0,
        .errors = 0,
        .elapsed_sec = 0,
        .throughput = 0,
        .unit = "MB/sec"
    };

    printf("\n=== %s ===\n", result.name);
    printf("  Message size: %zu bytes\n", msg_size);

    /* Allocate test buffer */
    char* send_buf = malloc(msg_size);
    char* recv_buf = malloc(msg_size);
    if (!send_buf || !recv_buf) {
        printf("  Failed to allocate buffers\n");
        result.errors = 1;
        free(send_buf);
        free(recv_buf);
        return result;
    }

    /* Initialize send buffer */
    memset(send_buf, 0xAA + g_node_id, msg_size);
    memset(recv_buf, 0, msg_size);

    /* Reset sync */
    g_local_shared->remote_ready = 0;
    pgas_fence(ctx, PGAS_CONSISTENCY_SEQ_CST);

    /* Create peer data pointer (using peer's known offset) */
    pgas_ptr_t peer_data = {
        .node_id = g_peer_id,
        .segment_id = 0,
        .offset = g_peer_region.offset + offsetof(shared_region_t, data),
        .flags = 0
    };

    pgas_ptr_t local_data_ptr = {
        .node_id = g_node_id,
        .segment_id = 0,
        .offset = g_local_region.offset + offsetof(shared_region_t, data),
        .flags = 0
    };
    (void)local_data_ptr;  /* Unused in current test */

    pgas_ptr_t peer_ready = {
        .node_id = g_peer_id,
        .segment_id = 0,
        .offset = g_peer_region.offset + offsetof(shared_region_t, remote_ready),
        .flags = 0
    };

    /* Signal ready */
    g_local_shared->remote_ready = 1;
    pgas_fence(ctx, PGAS_CONSISTENCY_SEQ_CST);

    /* Wait for peer */
    printf("  Waiting for peer...\n");
    uint64_t peer_ready_val = 0;
    double wait_start = get_time_sec();
    while (peer_ready_val == 0 && g_running) {
        pgas_get(ctx, &peer_ready_val, peer_ready, sizeof(uint64_t));
        if (get_time_sec() - wait_start > SYNC_TIMEOUT_SEC) {
            printf("  Timeout waiting for peer\n");
            result.errors = 1;
            free(send_buf);
            free(recv_buf);
            return result;
        }
        usleep(1000);
    }
    printf("  Peer ready, starting test...\n");

    size_t total_bytes = 0;
    double start = get_time_sec();

    for (int i = 0; i < iterations && g_running; i++) {
        /* Write to peer's data region */
        pgas_put(ctx, peer_data, send_buf, msg_size);
        total_bytes += msg_size;

        /* Read from peer's data region */
        pgas_get(ctx, recv_buf, peer_data, msg_size);
        total_bytes += msg_size;
    }

    pgas_fence(ctx, PGAS_CONSISTENCY_SEQ_CST);
    result.elapsed_sec = get_time_sec() - start;

    result.throughput = (total_bytes / result.elapsed_sec) / (1024 * 1024);
    result.passed = 1;

    printf("  Iterations: %d\n", iterations);
    printf("  Total data: %.2f MB\n", total_bytes / (1024.0 * 1024.0));
    printf("  Time: %.3f sec\n", result.elapsed_sec);
    printf("  Throughput: %.2f MB/sec\n", result.throughput);

    free(send_buf);
    free(recv_buf);
    return result;
}

/*
 * Test 4: Message passing (producer-consumer)
 */
static test_result_t test_message_passing(pgas_context_t* ctx, int num_messages) {
    test_result_t result = {
        .name = "Message Passing Test",
        .passed = 0,
        .errors = 0,
        .elapsed_sec = 0,
        .throughput = 0,
        .unit = "K msgs/sec"
    };

    printf("\n=== %s ===\n", result.name);
    printf("  Node %d: %s\n", g_node_id, g_node_id == 0 ? "Producer" : "Consumer");

    /* Use counter as message sequence number */
    g_local_shared->counter = 0;
    g_local_shared->remote_ready = 0;
    pgas_fence(ctx, PGAS_CONSISTENCY_SEQ_CST);

    pgas_ptr_t peer_counter = {
        .node_id = g_peer_id,
        .segment_id = 0,
        .offset = g_peer_region.offset + offsetof(shared_region_t, counter),
        .flags = 0
    };

    pgas_ptr_t peer_ready = {
        .node_id = g_peer_id,
        .segment_id = 0,
        .offset = g_peer_region.offset + offsetof(shared_region_t, remote_ready),
        .flags = 0
    };

    /* Signal ready */
    g_local_shared->remote_ready = 1;
    pgas_fence(ctx, PGAS_CONSISTENCY_SEQ_CST);

    /* Wait for peer */
    printf("  Waiting for peer...\n");
    uint64_t peer_ready_val = 0;
    double wait_start = get_time_sec();
    while (peer_ready_val == 0 && g_running) {
        pgas_get(ctx, &peer_ready_val, peer_ready, sizeof(uint64_t));
        if (get_time_sec() - wait_start > SYNC_TIMEOUT_SEC) {
            printf("  Timeout waiting for peer\n");
            result.errors = 1;
            return result;
        }
        usleep(1000);
    }
    printf("  Peer ready, starting test...\n");

    double start = get_time_sec();

    if (g_node_id == 0) {
        /* Producer: send messages */
        for (int i = 0; i < num_messages && g_running; i++) {
            uint64_t msg = i + 1;
            pgas_put(ctx, peer_counter, &msg, sizeof(uint64_t));
            pgas_fence(ctx, PGAS_CONSISTENCY_SEQ_CST);
        }
    } else {
        /* Consumer: receive messages */
        uint64_t expected = 1;
        uint64_t received = 0;

        while (expected <= (uint64_t)num_messages && g_running) {
            received = g_local_shared->counter;
            if (received >= expected) {
                expected = received + 1;
            }
        }

        if (received != (uint64_t)num_messages) {
            printf("  Warning: received %lu messages, expected %d\n", received, num_messages);
        }
    }

    result.elapsed_sec = get_time_sec() - start;
    result.throughput = (num_messages / result.elapsed_sec) / 1e3;
    result.passed = (result.errors == 0);

    printf("  Messages: %d\n", num_messages);
    printf("  Time: %.3f sec\n", result.elapsed_sec);
    printf("  Throughput: %.2f K msgs/sec\n", result.throughput);

    return result;
}

static void print_usage(const char* prog) {
    printf("Usage: %s [options]\n\n", prog);
    printf("PGAS Two-Node Self-Loop Test\n\n");
    printf("Options:\n");
    printf("  -c, --config FILE   PGAS config file (required)\n");
    printf("  -i, --iterations N  Number of iterations (default: %d)\n", DEFAULT_ITERATIONS);
    printf("  -s, --size BYTES    Message size for bulk test (default: %d)\n", DEFAULT_MESSAGE_SIZE);
    printf("  -t, --test TEST     Run specific test: ping|atomic|bulk|msg|all (default: all)\n");
    printf("  -v, --verbose       Enable verbose output\n");
    printf("  -h, --help          Show this help\n");
    printf("\nExample (run in two terminals):\n");
    printf("  Terminal 1: %s -c config/node0.conf\n", prog);
    printf("  Terminal 2: %s -c config/node1.conf\n", prog);
}

int main(int argc, char* argv[]) {
    const char* config_file = NULL;
    int iterations = DEFAULT_ITERATIONS;
    size_t msg_size = DEFAULT_MESSAGE_SIZE;
    const char* test_name = "all";
    int verbose = 0;

    static struct option long_options[] = {
        {"config",     required_argument, 0, 'c'},
        {"iterations", required_argument, 0, 'i'},
        {"size",       required_argument, 0, 's'},
        {"test",       required_argument, 0, 't'},
        {"verbose",    no_argument,       0, 'v'},
        {"help",       no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "c:i:s:t:vh", long_options, NULL)) != -1) {
        switch (opt) {
            case 'c':
                config_file = optarg;
                break;
            case 'i':
                iterations = atoi(optarg);
                break;
            case 's':
                msg_size = atoi(optarg);
                break;
            case 't':
                test_name = optarg;
                break;
            case 'v':
                verbose = 1;
                break;
            case 'h':
            default:
                print_usage(argv[0]);
                return (opt == 'h') ? 0 : 1;
        }
    }

    if (!config_file) {
        fprintf(stderr, "Error: Configuration file required\n\n");
        print_usage(argv[0]);
        return 1;
    }

    /* Setup signal handlers */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    printf("========================================\n");
    printf("  PGAS Two-Node Self-Loop Test\n");
    printf("========================================\n\n");

    /* Initialize PGAS */
    printf("Initializing PGAS with config: %s\n", config_file);
    if (pgas_init(&g_ctx, config_file) != 0) {
        fprintf(stderr, "Failed to initialize PGAS\n");
        return 1;
    }

    g_node_id = pgas_my_node(&g_ctx);
    g_num_nodes = pgas_num_nodes(&g_ctx);
    g_peer_id = (g_node_id + 1) % g_num_nodes;

    printf("  Local node: %d\n", g_node_id);
    printf("  Peer node: %d\n", g_peer_id);
    printf("  Total nodes: %d\n", g_num_nodes);

    if (g_num_nodes < 2) {
        fprintf(stderr, "Error: Need at least 2 nodes for this test\n");
        pgas_finalize(&g_ctx);
        return 1;
    }

    /* Set up regions at fixed offsets (so both nodes know each other's addresses) */
    printf("\nAllocating shared region...\n");

    /* Determine local and peer region offsets based on node ID */
    uint64_t local_offset = (g_node_id == 0) ? NODE0_REGION_OFFSET : NODE1_REGION_OFFSET;
    uint64_t peer_offset = (g_peer_id == 0) ? NODE0_REGION_OFFSET : NODE1_REGION_OFFSET;

    /* Set up local region pointer */
    g_local_region.node_id = g_node_id;
    g_local_region.segment_id = 0;
    g_local_region.offset = local_offset;
    g_local_region.flags = 0;

    /* Set up peer region pointer */
    g_peer_region.node_id = g_peer_id;
    g_peer_region.segment_id = 0;
    g_peer_region.offset = peer_offset;
    g_peer_region.flags = 0;

    /* Get local pointer to our fixed offset region */
    g_local_shared = (shared_region_t*)pgas_local_ptr(&g_ctx, g_local_region);
    if (!g_local_shared) {
        fprintf(stderr, "Failed to get local pointer at offset 0x%lx\n", local_offset);
        pgas_finalize(&g_ctx);
        return 1;
    }

    /* Initialize shared region */
    memset(g_local_shared, 0, sizeof(shared_region_t));
    pgas_fence(&g_ctx, PGAS_CONSISTENCY_SEQ_CST);

    printf("  Shared region allocated at offset 0x%lx\n", g_local_region.offset);
    printf("  Peer region at offset 0x%lx\n", g_peer_region.offset);

    /* Run tests */
    test_result_t results[4];
    int num_tests = 0;
    int total_errors = 0;

    int run_all = (strcmp(test_name, "all") == 0);

    if (run_all || strcmp(test_name, "ping") == 0) {
        results[num_tests] = test_ping_pong(&g_ctx, iterations);
        total_errors += results[num_tests].errors;
        num_tests++;
    }

    if (run_all || strcmp(test_name, "atomic") == 0) {
        results[num_tests] = test_remote_atomics(&g_ctx, iterations);
        total_errors += results[num_tests].errors;
        num_tests++;
    }

    if (run_all || strcmp(test_name, "bulk") == 0) {
        results[num_tests] = test_bulk_transfer(&g_ctx, iterations / 10, msg_size);
        total_errors += results[num_tests].errors;
        num_tests++;
    }

    if (run_all || strcmp(test_name, "msg") == 0) {
        results[num_tests] = test_message_passing(&g_ctx, iterations);
        total_errors += results[num_tests].errors;
        num_tests++;
    }

    /* Print PGAS stats */
    printf("\n=== PGAS Statistics ===\n");
    pgas_stats_t stats;
    pgas_get_stats(&g_ctx, &stats);
    printf("  Local reads: %lu, writes: %lu\n", stats.local_reads, stats.local_writes);
    printf("  Remote reads: %lu, writes: %lu\n", stats.remote_reads, stats.remote_writes);
    printf("  Atomics: %lu\n", stats.atomics);
    printf("  Bytes transferred: %lu\n", stats.bytes_transferred);

    /* Cleanup (no need to free - we used fixed offset, not allocated) */
    pgas_finalize(&g_ctx);

    /* Summary */
    printf("\n========================================\n");
    printf("  Test Summary (Node %d)\n", g_node_id);
    printf("========================================\n");

    int passed = 0;
    for (int i = 0; i < num_tests; i++) {
        printf("  [%s] %s: %.2f %s\n",
               results[i].passed ? "PASS" : "FAIL",
               results[i].name,
               results[i].throughput,
               results[i].unit);
        if (results[i].passed) passed++;
    }

    printf("\n  Passed: %d/%d\n", passed, num_tests);
    printf("  Status: %s\n", total_errors == 0 ? "SUCCESS" : "FAILED");

    return total_errors > 0 ? 1 : 0;
}
