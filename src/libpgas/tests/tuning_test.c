/*
 * PGAS Workload Tuning Test
 *
 * Demonstrates the tuning API for different workload profiles:
 * - MCF: Pointer-chasing, latency sensitive
 * - LLAMA: Bandwidth-bound LLM inference
 * - GROMACS: Molecular dynamics with neighbor lists
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pgas/pgas.h>

void print_tuning(const char* profile_name, const pgas_tuning_t* tuning) {
    const char* affinity_names[] = {"LOCAL", "REMOTE", "INTERLEAVE", "REPLICATE"};
    const char* partition_names[] = {"BLOCK", "CYCLIC", "BLOCK_CYCLIC", "HASH", "CUSTOM"};
    const char* prefetch_names[] = {"NONE", "SEQUENTIAL", "STRIDED", "AGGRESSIVE", "NEIGHBOR_LIST"};
    const char* consistency_names[] = {"RELAXED", "RELEASE", "ACQUIRE", "SEQ_CST"};

    printf("\n=== %s Profile ===\n", profile_name);
    printf("Memory Configuration:\n");
    printf("  Affinity:        %s\n", affinity_names[tuning->memory_affinity]);
    printf("  Partition:       %s\n", partition_names[tuning->partition_scheme]);
    printf("  Cache align:     %s\n", tuning->cache_line_align ? "yes" : "no");
    printf("  NUMA bind:       %s\n", tuning->numa_bind ? "yes" : "no");
    printf("\nTransfer Settings:\n");
    printf("  Batch size:      %zu\n", tuning->batch_size);
    printf("  Transfer size:   %zu bytes\n", tuning->transfer_size);
    printf("  Prefetch mode:   %s\n", prefetch_names[tuning->prefetch_mode]);
    printf("\nConsistency & Threading:\n");
    printf("  Consistency:     %s\n", consistency_names[tuning->consistency]);
    printf("  Threads:         %d\n", tuning->num_threads);
    printf("\nOptimization Hints:\n");
    printf("  BW priority:     %s\n", tuning->bandwidth_priority ? "yes" : "no");
    printf("  Async transfer:  %s\n", tuning->async_transfer ? "yes" : "no");
}

int main(int argc, char* argv[]) {
    printf("╔════════════════════════════════════════════════════════════════╗\n");
    printf("║          PGAS Workload Tuning Profile Comparison               ║\n");
    printf("╚════════════════════════════════════════════════════════════════╝\n");

    // Get and print all predefined profiles
    const pgas_tuning_t* default_tuning = pgas_get_default_tuning(PGAS_PROFILE_DEFAULT);
    print_tuning("DEFAULT", default_tuning);

    const pgas_tuning_t* mcf_tuning = pgas_get_default_tuning(PGAS_PROFILE_MCF);
    print_tuning("MCF (SPEC CPU)", mcf_tuning);

    const pgas_tuning_t* llama_tuning = pgas_get_default_tuning(PGAS_PROFILE_LLAMA);
    print_tuning("LLAMA (LLM Inference)", llama_tuning);

    const pgas_tuning_t* gromacs_tuning = pgas_get_default_tuning(PGAS_PROFILE_GROMACS);
    print_tuning("GROMACS (Molecular Dynamics)", gromacs_tuning);

    const pgas_tuning_t* graph_tuning = pgas_get_default_tuning(PGAS_PROFILE_GRAPH);
    print_tuning("GRAPH (BFS, PageRank)", graph_tuning);

    // Profile comparison summary
    printf("\n╔════════════════════════════════════════════════════════════════╗\n");
    printf("║                    Profile Comparison Summary                   ║\n");
    printf("╚════════════════════════════════════════════════════════════════╝\n");
    printf("\n%-15s %-12s %-10s %-12s %-8s\n",
           "Profile", "Affinity", "BatchSz", "Prefetch", "BW Prio");
    printf("%-15s %-12s %-10s %-12s %-8s\n",
           "-------", "--------", "-------", "--------", "-------");
    printf("%-15s %-12s %-10zu %-12s %-8s\n",
           "MCF", "LOCAL", mcf_tuning->batch_size, "AGGRESSIVE", "no");
    printf("%-15s %-12s %-10zu %-12s %-8s\n",
           "LLAMA", "INTERLEAVE", llama_tuning->batch_size, "SEQUENTIAL", "yes");
    printf("%-15s %-12s %-10zu %-12s %-8s\n",
           "GROMACS", "LOCAL", gromacs_tuning->batch_size, "NEIGHBOR", "no");
    printf("%-15s %-12s %-10zu %-12s %-8s\n",
           "GRAPH", "LOCAL", graph_tuning->batch_size, "NONE", "no");

    printf("\n=== Tuning Rationale ===\n\n");

    printf("MCF (SPEC CPU 429.mcf/605.mcf):\n");
    printf("  - Vehicle scheduling with pointer-chasing access patterns\n");
    printf("  - High L3 cache miss rate (MPKI 2-3)\n");
    printf("  - Two critical 2GB objects dominate performance\n");
    printf("  - Tuning: LOCAL affinity, aggressive prefetch, latency focus\n");
    printf("  - Research shows LOCAL placement reduces slowdown 13%->2%%\n\n");

    printf("LLAMA (LLM Inference):\n");
    printf("  - Memory bandwidth bound at decode phase\n");
    printf("  - Low arithmetic intensity (<25 even at batch=32)\n");
    printf("  - Large model weights (7B=14GB, 70B=140GB, 405B=629GB+)\n");
    printf("  - Tuning: INTERLEAVE for bandwidth aggregation\n");
    printf("  - Large batches (4096) and 1MB transfers for weight loading\n");
    printf("  - Target >50%% Model Bandwidth Utilization (MBU)\n\n");

    printf("GROMACS (Molecular Dynamics):\n");
    printf("  - Neighbor-list based force calculations\n");
    printf("  - 3D domain decomposition with halo exchange\n");
    printf("  - NUMA-aware with OpenMP parallelization\n");
    printf("  - Tuning: LOCAL for domain data, async halo exchange\n");
    printf("  - NEIGHBOR_LIST prefetch using MD neighbor list as hint\n\n");

    printf("Graph Analytics (BFS, PageRank):\n");
    printf("  - Irregular, frontier-driven access patterns\n");
    printf("  - Vertex/edge data distributed across nodes\n");
    printf("  - Tuning: LOCAL affinity, no prefetch (unpredictable)\n");
    printf("  - Small transfer sizes for vertex data\n\n");

    return 0;
}
