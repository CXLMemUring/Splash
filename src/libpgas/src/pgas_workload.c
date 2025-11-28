/*
 * PGAS Workload Abstraction Implementation
 *
 * Provides common workload management functions and a workload registry
 * for supporting multiple workload types (graph, key-value, array, etc.)
 */

#include "pgas_workload.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

/* Maximum number of registered workload types */
#define MAX_REGISTERED_WORKLOADS 32

/* Workload registry entry */
typedef struct {
    char name[64];
    pgas_workload_type_t type;
    const pgas_workload_ops_t* ops;
    bool in_use;
} workload_registry_entry_t;

/* Global workload registry */
static workload_registry_entry_t g_registry[MAX_REGISTERED_WORKLOADS];
static int g_registry_count = 0;
static pthread_mutex_t g_registry_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_registry_initialized = false;

/* Initialize registry on first use */
static void ensure_registry_initialized(void) {
    if (!g_registry_initialized) {
        pthread_mutex_lock(&g_registry_lock);
        if (!g_registry_initialized) {
            memset(g_registry, 0, sizeof(g_registry));
            g_registry_count = 0;
            g_registry_initialized = true;
        }
        pthread_mutex_unlock(&g_registry_lock);
    }
}

/* Get current time in seconds */
static double get_time_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/*
 * Create a new workload instance
 */
pgas_workload_t* pgas_workload_create(pgas_workload_type_t type, const char* name) {
    pgas_workload_t* workload = calloc(1, sizeof(pgas_workload_t));
    if (!workload) {
        return NULL;
    }

    workload->name = name ? strdup(name) : NULL;
    workload->type = type;
    workload->state = PGAS_WORKLOAD_STATE_UNINITIALIZED;
    workload->ctx = NULL;
    workload->ops = NULL;
    workload->private_data = NULL;

    /* Try to find registered ops for this type/name */
    ensure_registry_initialized();
    pthread_mutex_lock(&g_registry_lock);
    for (int i = 0; i < g_registry_count; i++) {
        if (g_registry[i].in_use) {
            /* Match by name first if provided */
            if (name && strcmp(g_registry[i].name, name) == 0) {
                workload->ops = g_registry[i].ops;
                break;
            }
            /* Fall back to type match */
            if (!name && g_registry[i].type == type) {
                workload->ops = g_registry[i].ops;
                break;
            }
        }
    }
    pthread_mutex_unlock(&g_registry_lock);

    return workload;
}

/*
 * Initialize workload with PGAS context and configuration
 */
int pgas_workload_init(pgas_workload_t* workload, pgas_context_t* ctx,
                       const pgas_workload_config_t* config) {
    if (!workload || !ctx) {
        return -1;
    }

    workload->ctx = ctx;
    if (config) {
        workload->config = *config;
    }

    /* Clear statistics */
    memset(&workload->stats, 0, sizeof(workload->stats));

    /* Call workload-specific init if available */
    if (workload->ops && workload->ops->init) {
        int ret = workload->ops->init(workload, ctx, config);
        if (ret != 0) {
            return ret;
        }
    }

    workload->state = PGAS_WORKLOAD_STATE_INITIALIZED;
    return 0;
}

/*
 * Load data from source
 */
int pgas_workload_load(pgas_workload_t* workload, const char* source) {
    if (!workload) {
        return -1;
    }

    PGAS_WORKLOAD_CHECK_STATE(workload, PGAS_WORKLOAD_STATE_INITIALIZED);

    if (workload->ops && workload->ops->load_data) {
        return workload->ops->load_data(workload, source);
    }

    /* No load function - that's okay for some workloads */
    return 0;
}

/*
 * Partition data across nodes
 */
int pgas_workload_partition(pgas_workload_t* workload, pgas_partition_scheme_t scheme) {
    if (!workload) {
        return -1;
    }

    if (workload->ops && workload->ops->partition) {
        return workload->ops->partition(workload, scheme);
    }

    /* Default: no partitioning needed */
    return 0;
}

/*
 * Run the complete workload
 */
int pgas_workload_run(pgas_workload_t* workload) {
    if (!workload) {
        return -1;
    }

    if (workload->state != PGAS_WORKLOAD_STATE_INITIALIZED &&
        workload->state != PGAS_WORKLOAD_STATE_PAUSED) {
        return -1;
    }

    double start_time = get_time_sec();
    PGAS_WORKLOAD_SET_STATE(workload, PGAS_WORKLOAD_STATE_RUNNING);

    int result = 0;
    if (workload->ops && workload->ops->run) {
        result = workload->ops->run(workload);
    } else if (workload->ops && workload->ops->run_iteration) {
        /* Default: run iterations until config limit or convergence */
        int max_iters = workload->config.num_iterations > 0 ?
                        workload->config.num_iterations : 100;

        for (int i = 0; i < max_iters; i++) {
            result = workload->ops->run_iteration(workload);
            if (result != 0) break;

            workload->stats.iterations_completed++;

            /* Check convergence if available */
            if (workload->ops->check_convergence) {
                double residual;
                if (workload->ops->check_convergence(workload, &residual) == 0) {
                    break;  /* Converged */
                }
            }
        }
    }

    workload->stats.elapsed_time_sec = get_time_sec() - start_time;

    if (result == 0) {
        PGAS_WORKLOAD_SET_STATE(workload, PGAS_WORKLOAD_STATE_COMPLETED);
    } else {
        PGAS_WORKLOAD_SET_STATE(workload, PGAS_WORKLOAD_STATE_ERROR);
    }

    /* Collect PGAS stats */
    if (workload->ctx) {
        pgas_get_stats(workload->ctx, &workload->stats.pgas_stats);
    }

    return result;
}

/*
 * Run single iteration
 */
int pgas_workload_step(pgas_workload_t* workload) {
    if (!workload) {
        return -1;
    }

    if (workload->state != PGAS_WORKLOAD_STATE_INITIALIZED &&
        workload->state != PGAS_WORKLOAD_STATE_RUNNING &&
        workload->state != PGAS_WORKLOAD_STATE_PAUSED) {
        return -1;
    }

    PGAS_WORKLOAD_SET_STATE(workload, PGAS_WORKLOAD_STATE_RUNNING);

    int result = 0;
    if (workload->ops && workload->ops->run_iteration) {
        double start_time = get_time_sec();
        result = workload->ops->run_iteration(workload);
        workload->stats.elapsed_time_sec += get_time_sec() - start_time;
        workload->stats.iterations_completed++;
    }

    return result;
}

/*
 * Synchronize across nodes
 */
int pgas_workload_sync(pgas_workload_t* workload) {
    if (!workload || !workload->ctx) {
        return -1;
    }

    if (workload->ops && workload->ops->sync) {
        return workload->ops->sync(workload);
    }

    /* Default: use PGAS barrier */
    pgas_barrier(workload->ctx);
    return 0;
}

/*
 * Get workload statistics
 */
int pgas_workload_get_stats(pgas_workload_t* workload, pgas_workload_stats_t* stats) {
    if (!workload || !stats) {
        return -1;
    }

    /* Get latest PGAS stats */
    if (workload->ctx) {
        pgas_get_stats(workload->ctx, &workload->stats.pgas_stats);
    }

    /* Call workload-specific stats if available */
    if (workload->ops && workload->ops->get_stats) {
        return workload->ops->get_stats(workload, stats);
    }

    /* Return base stats */
    *stats = workload->stats;

    /* Calculate throughput if not set */
    if (stats->throughput == 0 && stats->elapsed_time_sec > 0) {
        if (stats->operations_performed > 0) {
            stats->throughput = stats->operations_performed / stats->elapsed_time_sec;
        } else if (stats->bytes_processed > 0) {
            stats->throughput = stats->bytes_processed / stats->elapsed_time_sec;
        }
    }

    return 0;
}

/*
 * Destroy workload instance
 */
void pgas_workload_destroy(pgas_workload_t* workload) {
    if (!workload) {
        return;
    }

    /* Call workload-specific cleanup */
    if (workload->ops && workload->ops->finalize) {
        workload->ops->finalize(workload);
    }

    /* Free name if allocated */
    if (workload->name) {
        free((void*)workload->name);
    }

    free(workload);
}

/*
 * Register a custom workload type
 */
int pgas_workload_register(const char* name, pgas_workload_type_t type,
                           const pgas_workload_ops_t* ops) {
    if (!name || !ops) {
        return -1;
    }

    ensure_registry_initialized();

    pthread_mutex_lock(&g_registry_lock);

    /* Check if already registered */
    for (int i = 0; i < g_registry_count; i++) {
        if (g_registry[i].in_use && strcmp(g_registry[i].name, name) == 0) {
            /* Update existing entry */
            g_registry[i].type = type;
            g_registry[i].ops = ops;
            pthread_mutex_unlock(&g_registry_lock);
            return 0;
        }
    }

    /* Find free slot */
    if (g_registry_count >= MAX_REGISTERED_WORKLOADS) {
        pthread_mutex_unlock(&g_registry_lock);
        return -1;
    }

    /* Add new entry */
    int idx = g_registry_count++;
    strncpy(g_registry[idx].name, name, sizeof(g_registry[idx].name) - 1);
    g_registry[idx].type = type;
    g_registry[idx].ops = ops;
    g_registry[idx].in_use = true;

    pthread_mutex_unlock(&g_registry_lock);
    return 0;
}

/*
 * Get registered workload by name
 */
const pgas_workload_ops_t* pgas_workload_get_ops(const char* name) {
    if (!name) {
        return NULL;
    }

    ensure_registry_initialized();

    pthread_mutex_lock(&g_registry_lock);
    for (int i = 0; i < g_registry_count; i++) {
        if (g_registry[i].in_use && strcmp(g_registry[i].name, name) == 0) {
            const pgas_workload_ops_t* ops = g_registry[i].ops;
            pthread_mutex_unlock(&g_registry_lock);
            return ops;
        }
    }
    pthread_mutex_unlock(&g_registry_lock);

    return NULL;
}

/*
 * List registered workloads
 */
int pgas_workload_list(char** names, int* count) {
    if (!count) {
        return -1;
    }

    ensure_registry_initialized();

    pthread_mutex_lock(&g_registry_lock);

    int active_count = 0;
    for (int i = 0; i < g_registry_count; i++) {
        if (g_registry[i].in_use) {
            if (names && active_count < *count) {
                names[active_count] = g_registry[i].name;
            }
            active_count++;
        }
    }

    *count = active_count;
    pthread_mutex_unlock(&g_registry_lock);

    return 0;
}

/*
 * Print workload statistics
 */
void pgas_workload_print_stats(pgas_workload_t* workload) {
    if (!workload) {
        return;
    }

    pgas_workload_stats_t stats;
    pgas_workload_get_stats(workload, &stats);

    printf("\n=== Workload Statistics: %s ===\n", workload->name ? workload->name : "unnamed");
    printf("  State: ");
    switch (workload->state) {
        case PGAS_WORKLOAD_STATE_UNINITIALIZED: printf("uninitialized\n"); break;
        case PGAS_WORKLOAD_STATE_INITIALIZED: printf("initialized\n"); break;
        case PGAS_WORKLOAD_STATE_RUNNING: printf("running\n"); break;
        case PGAS_WORKLOAD_STATE_PAUSED: printf("paused\n"); break;
        case PGAS_WORKLOAD_STATE_COMPLETED: printf("completed\n"); break;
        case PGAS_WORKLOAD_STATE_ERROR: printf("error\n"); break;
    }
    printf("  Iterations completed: %lu\n", stats.iterations_completed);
    printf("  Operations performed: %lu\n", stats.operations_performed);
    printf("  Bytes processed: %lu\n", stats.bytes_processed);
    printf("  Elapsed time: %.3f sec\n", stats.elapsed_time_sec);
    printf("  Throughput: %.2f ops/sec\n", stats.throughput);
    printf("\n  PGAS Statistics:\n");
    printf("    Local reads: %lu, writes: %lu\n",
           stats.pgas_stats.local_reads, stats.pgas_stats.local_writes);
    printf("    Remote reads: %lu, writes: %lu\n",
           stats.pgas_stats.remote_reads, stats.pgas_stats.remote_writes);
    printf("    Atomics: %lu, Barriers: %lu\n",
           stats.pgas_stats.atomics, stats.pgas_stats.barriers);
    printf("    Bytes transferred: %lu\n", stats.pgas_stats.bytes_transferred);
}
