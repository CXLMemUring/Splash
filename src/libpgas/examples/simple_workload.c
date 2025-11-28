/*
 * Simple PGAS Workload Example
 *
 * Demonstrates basic usage of the PGAS workload abstraction.
 * This example implements a simple distributed counter workload.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>

#include "pgas.h"
#include "pgas_workload.h"

/* Workload-specific data */
typedef struct {
    pgas_ptr_t counter_ptr;
    uint64_t local_count;
    int iterations;
} counter_workload_data_t;

/* Initialize the counter workload */
static int counter_init(pgas_workload_t* workload, pgas_context_t* ctx,
                        const pgas_workload_config_t* config) {
    counter_workload_data_t* data = calloc(1, sizeof(counter_workload_data_t));
    if (!data) return -1;

    data->iterations = config ? config->num_iterations : 1000;
    data->local_count = 0;

    /* Allocate distributed counter on node 0 */
    if (pgas_my_node(ctx) == 0) {
        data->counter_ptr = pgas_alloc(ctx, sizeof(uint64_t), PGAS_AFFINITY_LOCAL);
        if (pgas_ptr_is_null(data->counter_ptr)) {
            free(data);
            return -1;
        }
        /* Initialize to zero */
        uint64_t* local = pgas_local_ptr(ctx, data->counter_ptr);
        *local = 0;
    }

    workload->private_data = data;
    return 0;
}

/* Run one iteration - increment counter */
static int counter_run_iteration(pgas_workload_t* workload) {
    counter_workload_data_t* data = workload->private_data;

    /* Atomic increment on node 0's counter */
    pgas_ptr_t counter = {
        .node_id = 0,
        .segment_id = 0,
        .offset = data->counter_ptr.offset,
        .flags = 0
    };

    pgas_atomic_fetch_add(workload->ctx, counter, 1);
    data->local_count++;
    workload->stats.operations_performed++;

    return 0;
}

/* Run complete workload */
static int counter_run(pgas_workload_t* workload) {
    counter_workload_data_t* data = workload->private_data;

    printf("Node %d: Running %d iterations\n",
           pgas_my_node(workload->ctx), data->iterations);

    for (int i = 0; i < data->iterations; i++) {
        counter_run_iteration(workload);
    }

    /* Barrier to ensure all nodes complete */
    pgas_barrier(workload->ctx);

    /* Node 0 reads final value */
    if (pgas_my_node(workload->ctx) == 0) {
        uint64_t* local = pgas_local_ptr(workload->ctx, data->counter_ptr);
        printf("Final counter value: %lu (expected: %lu)\n",
               *local, (uint64_t)data->iterations * pgas_num_nodes(workload->ctx));
    }

    return 0;
}

/* Cleanup */
static void counter_finalize(pgas_workload_t* workload) {
    counter_workload_data_t* data = workload->private_data;
    if (data) {
        if (pgas_my_node(workload->ctx) == 0 && !pgas_ptr_is_null(data->counter_ptr)) {
            pgas_free(workload->ctx, data->counter_ptr);
        }
        free(data);
    }
}

/* Workload operations structure */
static const pgas_workload_ops_t counter_ops = {
    .init = counter_init,
    .load_data = NULL,
    .partition = NULL,
    .run_iteration = counter_run_iteration,
    .run = counter_run,
    .sync = NULL,
    .check_convergence = NULL,
    .get_results = NULL,
    .get_stats = NULL,
    .finalize = counter_finalize
};

static void print_usage(const char* prog) {
    printf("Usage: %s [options]\n\n", prog);
    printf("Simple PGAS Counter Workload Example\n\n");
    printf("Options:\n");
    printf("  -c, --config FILE   PGAS config file (required)\n");
    printf("  -i, --iterations N  Number of iterations (default: 1000)\n");
    printf("  -h, --help          Show this help\n");
}

int main(int argc, char* argv[]) {
    const char* config_file = NULL;
    int iterations = 1000;

    static struct option long_options[] = {
        {"config",     required_argument, 0, 'c'},
        {"iterations", required_argument, 0, 'i'},
        {"help",       no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "c:i:h", long_options, NULL)) != -1) {
        switch (opt) {
            case 'c': config_file = optarg; break;
            case 'i': iterations = atoi(optarg); break;
            case 'h':
            default:
                print_usage(argv[0]);
                return (opt == 'h') ? 0 : 1;
        }
    }

    if (!config_file) {
        fprintf(stderr, "Error: Config file required\n");
        print_usage(argv[0]);
        return 1;
    }

    /* Initialize PGAS */
    pgas_context_t ctx;
    if (pgas_init(&ctx, config_file) != 0) {
        fprintf(stderr, "Failed to initialize PGAS\n");
        return 1;
    }

    printf("=== Simple Counter Workload ===\n");
    printf("Node %d of %d\n", pgas_my_node(&ctx), pgas_num_nodes(&ctx));

    /* Register workload type */
    pgas_workload_register("counter", PGAS_WORKLOAD_CUSTOM, &counter_ops);

    /* Create workload */
    pgas_workload_t* workload = pgas_workload_create(PGAS_WORKLOAD_CUSTOM, "counter");
    if (!workload) {
        fprintf(stderr, "Failed to create workload\n");
        pgas_finalize(&ctx);
        return 1;
    }

    /* Configure workload */
    pgas_workload_config_t config = {
        .name = "counter",
        .type = PGAS_WORKLOAD_CUSTOM,
        .num_iterations = iterations
    };

    /* Initialize and run */
    if (pgas_workload_init(workload, &ctx, &config) != 0) {
        fprintf(stderr, "Failed to initialize workload\n");
        pgas_workload_destroy(workload);
        pgas_finalize(&ctx);
        return 1;
    }

    if (pgas_workload_run(workload) != 0) {
        fprintf(stderr, "Workload failed\n");
    }

    /* Print statistics */
    pgas_workload_print_stats(workload);

    /* Cleanup */
    pgas_workload_destroy(workload);
    pgas_finalize(&ctx);

    return 0;
}
