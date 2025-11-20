#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <getopt.h>

#include "pgas.h"
#include "cxl_memory.h"
#include "memcached_interceptor.h"

// Global state for signal handling
static mc_interceptor_t* g_interceptor = NULL;
static pgas_context_t* g_pgas_ctx = NULL;
static volatile bool g_running = true;

void signal_handler(int sig) {
    (void)sig;
    printf("\nShutting down...\n");
    g_running = false;
}

void print_usage(const char* prog) {
    printf("Usage: %s [options]\n\n", prog);
    printf("Memcached CXL Disaggregation Runtime\n\n");
    printf("Options:\n");
    printf("  -c, --config FILE       PGAS configuration file (required)\n");
    printf("  -m, --memcached PATH    Path to memcached binary (for uprobe attachment)\n");
    printf("  -p, --pid PID           Attach to running memcached process\n");
    printf("  -s, --cache-size SIZE   Local cache size in MB (default: 64)\n");
    printf("  -t, --hash-table SIZE   Hash table size (default: 1M)\n");
    printf("  -r, --replicate N       Enable replication with factor N\n");
    printf("  --no-cxl                Disable CXL disaggregation (local only)\n");
    printf("  --stats-interval SEC    Print stats every N seconds (default: 10)\n");
    printf("  -h, --help              Show this help message\n");
    printf("\n");
    printf("Example:\n");
    printf("  %s -c nodes.conf -m /usr/bin/memcached -s 128\n", prog);
    printf("\n");
    printf("Configuration file format (nodes.conf):\n");
    printf("  local_node_id=0\n");
    printf("  num_nodes=2\n");
    printf("  node0=192.168.1.10:5000:0x100000000:1073741824\n");
    printf("  node1=192.168.1.11:5000:0x100000000:1073741824\n");
}

int main(int argc, char* argv[]) {
    // Default configuration
    const char* config_file = NULL;
    const char* memcached_path = "/usr/bin/memcached";
    pid_t memcached_pid = 0;
    size_t cache_size_mb = 64;
    size_t hash_table_size = 1 << 20;  // 1M entries
    int replication_factor = 0;
    bool enable_cxl = true;
    int stats_interval = 10;

    // Parse command line
    static struct option long_options[] = {
        {"config", required_argument, 0, 'c'},
        {"memcached", required_argument, 0, 'm'},
        {"pid", required_argument, 0, 'p'},
        {"cache-size", required_argument, 0, 's'},
        {"hash-table", required_argument, 0, 't'},
        {"replicate", required_argument, 0, 'r'},
        {"no-cxl", no_argument, 0, 'n'},
        {"stats-interval", required_argument, 0, 'i'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "c:m:p:s:t:r:i:nh", long_options, NULL)) != -1) {
        switch (opt) {
            case 'c':
                config_file = optarg;
                break;
            case 'm':
                memcached_path = optarg;
                break;
            case 'p':
                memcached_pid = atoi(optarg);
                break;
            case 's':
                cache_size_mb = atoi(optarg);
                break;
            case 't':
                hash_table_size = atoi(optarg);
                break;
            case 'r':
                replication_factor = atoi(optarg);
                break;
            case 'n':
                enable_cxl = false;
                break;
            case 'i':
                stats_interval = atoi(optarg);
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

    printf("╔════════════════════════════════════════════════════════════════╗\n");
    printf("║     Memcached CXL Disaggregation with PGAS Abstraction        ║\n");
    printf("╚════════════════════════════════════════════════════════════════╝\n\n");

    // Setup signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Initialize PGAS
    printf("Initializing PGAS runtime...\n");
    g_pgas_ctx = malloc(sizeof(pgas_context_t));
    if (!g_pgas_ctx) {
        fprintf(stderr, "Failed to allocate PGAS context\n");
        return 1;
    }

    if (pgas_init(g_pgas_ctx, config_file) != 0) {
        fprintf(stderr, "Failed to initialize PGAS\n");
        free(g_pgas_ctx);
        return 1;
    }

    printf("PGAS initialized successfully\n");
    printf("  Local node: %d\n", pgas_my_node(g_pgas_ctx));
    printf("  Total nodes: %d\n", pgas_num_nodes(g_pgas_ctx));

    // Configure interceptor
    mc_interceptor_config_t interceptor_config = {
        .enable_cxl_disaggregation = enable_cxl,
        .enable_replication = (replication_factor > 0),
        .replication_factor = replication_factor,
        .local_cache_size = cache_size_mb * 1024 * 1024,
        .cxl_memory_size = 1ULL << 30,  // 1GB
        .cxl_allocation_ratio = 0.8,
        .prefetch_depth = 4,
        .enable_batching = true,
        .batch_size = 16,
        .batch_timeout_us = 100,
        .consistency_model = PGAS_CONSISTENCY_RELEASE,
        .enable_write_through = false,
        .hash_table_size = hash_table_size,
        .hash_seed = 0x9747b28c
    };

    // Initialize interceptor
    printf("\nInitializing memcached interceptor...\n");
    if (mc_interceptor_init(&g_interceptor, g_pgas_ctx, &interceptor_config) != 0) {
        fprintf(stderr, "Failed to initialize interceptor\n");
        pgas_finalize(g_pgas_ctx);
        free(g_pgas_ctx);
        return 1;
    }

    // Load and attach BPF programs
    printf("\nLoading BPF programs...\n");
    if (mc_interceptor_load_bpf(g_interceptor, memcached_path) != 0) {
        fprintf(stderr, "Warning: Failed to load BPF programs\n");
    }

    if (mc_interceptor_attach_uprobes(g_interceptor) != 0) {
        fprintf(stderr, "Warning: Failed to attach uprobes\n");
    }

    printf("\n");
    printf("Configuration:\n");
    printf("  CXL disaggregation: %s\n", enable_cxl ? "enabled" : "disabled");
    printf("  Local cache size: %zu MB\n", cache_size_mb);
    printf("  Hash table size: %zu entries\n", hash_table_size);
    printf("  Replication: %s", replication_factor > 0 ? "enabled" : "disabled");
    if (replication_factor > 0) {
        printf(" (factor: %d)", replication_factor);
    }
    printf("\n");
    printf("  Stats interval: %d seconds\n", stats_interval);
    printf("\nMemcached interception active. Press Ctrl+C to stop.\n\n");

    // Main loop
    int stats_counter = 0;
    while (g_running) {
        sleep(1);
        stats_counter++;

        if (stats_counter >= stats_interval) {
            mc_interceptor_print_stats(g_interceptor);

            // Also print PGAS stats
            pgas_stats_t pgas_stats;
            pgas_get_stats(g_pgas_ctx, &pgas_stats);
            printf("PGAS Statistics:\n");
            printf("  Local reads: %lu, writes: %lu\n",
                   pgas_stats.local_reads, pgas_stats.local_writes);
            printf("  Remote reads: %lu, writes: %lu\n",
                   pgas_stats.remote_reads, pgas_stats.remote_writes);
            printf("  Atomics: %lu, Barriers: %lu\n",
                   pgas_stats.atomics, pgas_stats.barriers);
            printf("  Bytes transferred: %lu\n", pgas_stats.bytes_transferred);
            printf("  Avg latency: %.2f μs\n\n", pgas_stats.avg_latency_us);

            stats_counter = 0;
        }
    }

    // Cleanup
    printf("Printing final statistics...\n");
    mc_interceptor_print_stats(g_interceptor);

    printf("Cleaning up...\n");
    mc_interceptor_finalize(g_interceptor);
    pgas_finalize(g_pgas_ctx);
    free(g_pgas_ctx);

    printf("Shutdown complete.\n");
    return 0;
}
