/*
 * MCF Integration Test
 * Simulates SPEC CPU2017 MCF (Network Simplex) memory access patterns on PGAS/CXL
 *
 * MCF characteristics:
 * - Two main data structures: node array (~2GB) and arc array (~2GB)
 * - High cache miss rate (MPKI 2-3)
 * - Pointer-chasing through tree structures
 * - Random access to arc costs during pivot selection
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>
#include <pgas/pgas.h>


#define KB (1024UL)
#define MB (1024UL * KB)
#define GB (1024UL * MB)

static inline double get_time_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/*
 * MCF Node structure (from network simplex)
 * Represents a node in the minimum cost flow network
 */
typedef struct mcf_node {
    int64_t potential;       // Node potential for reduced costs
    int64_t supply;          // Supply/demand at this node
    uint32_t first_out;      // First outgoing arc
    uint32_t first_in;       // First incoming arc
    uint32_t pred_arc;       // Predecessor arc in basis tree
    uint32_t depth;          // Depth in basis tree
    uint32_t thread;         // Thread index for tree traversal
    uint32_t rev_thread;     // Reverse thread
    struct mcf_node* pred;   // Predecessor node pointer (main chase target)
    struct mcf_node* child;  // First child in tree
    struct mcf_node* sibling;// Next sibling in tree
    uint32_t state;          // Node state flags
    char padding[12];        // Align to 96 bytes
} __attribute__((aligned(64))) mcf_node_t;

/*
 * MCF Arc structure
 * Represents an arc (edge) in the network
 */
typedef struct mcf_arc {
    int64_t cost;            // Arc cost
    int64_t flow;            // Current flow
    int64_t capacity;        // Arc capacity
    int64_t lower;           // Lower bound
    uint32_t tail;           // Source node index
    uint32_t head;           // Target node index
    uint32_t next_out;       // Next arc from tail
    uint32_t next_in;        // Next arc to head
    uint32_t state;          // Arc state (basic/nonbasic/lower/upper)
    char padding[20];        // Align to 64 bytes
} __attribute__((aligned(64))) mcf_arc_t;

typedef struct {
    double pivot_ops_per_sec;
    double tree_update_ops_per_sec;
    double reduced_cost_time;
    double pivot_time;
    double update_time;
    uint64_t total_pivots;
    uint64_t cache_misses_est;
} mcf_result_t;

/*
 * Simulate network simplex algorithm phases
 */
mcf_result_t run_mcf_simulation(size_t num_nodes, size_t num_arcs, int iterations) {
    mcf_result_t result = {0};

    printf("  Allocating MCF structures...\n");
    printf("    Nodes: %zu (%.1f MB)\n", num_nodes, num_nodes * sizeof(mcf_node_t) / (double)MB);
    printf("    Arcs:  %zu (%.1f MB)\n", num_arcs, num_arcs * sizeof(mcf_arc_t) / (double)MB);

    // Allocate using mmap (simulating CXL allocation pattern)
    mcf_node_t* nodes = mmap(NULL, num_nodes * sizeof(mcf_node_t),
                              PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    mcf_arc_t* arcs = mmap(NULL, num_arcs * sizeof(mcf_arc_t),
                            PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);

    if (nodes == MAP_FAILED || arcs == MAP_FAILED) {
        printf("  ERROR: Failed to allocate memory\n");
        return result;
    }

    // Initialize network structure
    printf("  Initializing network structure...\n");
    srand(42);

    // Initialize nodes
    for (size_t i = 0; i < num_nodes; i++) {
        nodes[i].potential = rand() % 10000;
        nodes[i].supply = (rand() % 200) - 100;
        nodes[i].first_out = rand() % num_arcs;
        nodes[i].first_in = rand() % num_arcs;
        nodes[i].depth = i % 100;
        nodes[i].thread = (i + 1) % num_nodes;
        nodes[i].rev_thread = (i + num_nodes - 1) % num_nodes;
        nodes[i].pred = (i > 0) ? &nodes[rand() % i] : NULL;
        nodes[i].child = NULL;
        nodes[i].sibling = NULL;
        nodes[i].state = 0;
    }

    // Build tree child/sibling structure (like basis tree)
    for (size_t i = 1; i < num_nodes; i++) {
        if (nodes[i].pred) {
            nodes[i].sibling = nodes[i].pred->child;
            nodes[i].pred->child = &nodes[i];
        }
    }

    // Initialize arcs
    for (size_t i = 0; i < num_arcs; i++) {
        arcs[i].cost = rand() % 1000;
        arcs[i].flow = 0;
        arcs[i].capacity = 100 + rand() % 9900;
        arcs[i].lower = 0;
        arcs[i].tail = rand() % num_nodes;
        arcs[i].head = rand() % num_nodes;
        arcs[i].next_out = rand() % num_arcs;
        arcs[i].next_in = rand() % num_arcs;
        arcs[i].state = rand() % 3;
    }

    __sync_synchronize();

    printf("  Running MCF simulation (%d iterations)...\n", iterations);

    volatile int64_t checksum = 0;
    uint64_t pivot_ops = 0;
    uint64_t update_ops = 0;

    /*
     * Phase 1: Reduced cost computation (like pricing)
     * Scan arcs and compute reduced costs using node potentials
     */
    double start = get_time_sec();

    for (int iter = 0; iter < iterations; iter++) {
        for (size_t i = 0; i < num_arcs; i++) {
            mcf_arc_t* arc = &arcs[i];
            // Access tail and head nodes (random access pattern)
            int64_t pi_tail = nodes[arc->tail].potential;
            int64_t pi_head = nodes[arc->head].potential;
            int64_t reduced_cost = arc->cost - pi_tail + pi_head;
            checksum += reduced_cost;
        }
    }

    result.reduced_cost_time = get_time_sec() - start;

    /*
     * Phase 2: Pivot selection (entering arc search)
     * MCF's most memory-intensive phase - searches for entering arc
     */
    start = get_time_sec();

    for (int iter = 0; iter < iterations; iter++) {
        // Block pivot selection (like SPEC MCF)
        size_t block_size = 500;
        int64_t best_violation = 0;
        size_t best_arc = 0;

        for (size_t block_start = 0; block_start < num_arcs; block_start += block_size) {
            size_t block_end = block_start + block_size;
            if (block_end > num_arcs) block_end = num_arcs;

            for (size_t i = block_start; i < block_end; i++) {
                mcf_arc_t* arc = &arcs[i];

                // Compute reduced cost (cache miss on nodes likely)
                int64_t rc = arc->cost - nodes[arc->tail].potential + nodes[arc->head].potential;

                // Check optimality condition based on arc state
                if (arc->state == 0 && rc < best_violation) {
                    best_violation = rc;
                    best_arc = i;
                }
                pivot_ops++;
            }

            // Early termination if good pivot found
            if (best_violation < -100) break;
        }

        checksum += best_arc;
        result.total_pivots++;
    }

    result.pivot_time = get_time_sec() - start;
    result.pivot_ops_per_sec = pivot_ops / result.pivot_time;

    /*
     * Phase 3: Tree update (basis change)
     * Pointer-chasing through tree structure
     */
    start = get_time_sec();

    for (int iter = 0; iter < iterations; iter++) {
        // Find path to root (pointer chasing)
        for (size_t start_node = 0; start_node < num_nodes; start_node += 100) {
            mcf_node_t* current = &nodes[start_node];

            // Chase pointers to root
            int depth = 0;
            while (current->pred && depth < 1000) {
                checksum += current->potential;
                current = current->pred;
                depth++;
                update_ops++;
            }

            // Visit subtree (child/sibling traversal)
            current = &nodes[start_node];
            mcf_node_t* stack[100];
            int sp = 0;

            if (current->child) {
                stack[sp++] = current->child;
            }

            while (sp > 0 && sp < 100) {
                current = stack[--sp];
                checksum += current->depth;
                update_ops++;

                if (current->sibling) {
                    stack[sp++] = current->sibling;
                }
                if (current->child && sp < 99) {
                    stack[sp++] = current->child;
                }
            }
        }
    }

    result.update_time = get_time_sec() - start;
    result.tree_update_ops_per_sec = update_ops / result.update_time;

    // Estimate cache misses (each pointer chase likely misses)
    result.cache_misses_est = pivot_ops + update_ops;

    // Prevent optimization
    if (checksum == 0) printf(".");

    // Cleanup
    munmap(nodes, num_nodes * sizeof(mcf_node_t));
    munmap(arcs, num_arcs * sizeof(mcf_arc_t));

    return result;
}

void print_header(const char* title) {
    printf("\n╔════════════════════════════════════════════════════════════════════════════╗\n");
    printf("║  %-74s ║\n", title);
    printf("╚════════════════════════════════════════════════════════════════════════════╝\n");
}

int main(int argc, char* argv[]) {
    size_t num_nodes = 1000000;  // 1M nodes (~96MB)
    size_t num_arcs = 5000000;   // 5M arcs (~320MB)
    int iterations = 5;

    // Parse args
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            num_nodes = atol(argv[++i]);
        } else if (strcmp(argv[i], "-a") == 0 && i + 1 < argc) {
            num_arcs = atol(argv[++i]);
        } else if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            iterations = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-h") == 0) {
            printf("Usage: %s [-n nodes] [-a arcs] [-i iterations]\n", argv[0]);
            printf("  Default: 1M nodes, 5M arcs, 5 iterations\n");
            return 0;
        }
    }

    printf("╔════════════════════════════════════════════════════════════════════════════╗\n");
    printf("║                    MCF Integration Benchmark                               ║\n");
    printf("║           Network Simplex Memory Access Patterns                           ║\n");
    printf("╚════════════════════════════════════════════════════════════════════════════╝\n");

    print_header("Running with DEFAULT Profile");
    pgas_context_t ctx;
    
    pgas_init(&ctx, NULL);
    pgas_load_profile(&ctx, PGAS_PROFILE_DEFAULT);

    const pgas_tuning_t* tuning = pgas_get_default_tuning(PGAS_PROFILE_DEFAULT);
    printf("  Batch: %zu, Transfer: %zu, Prefetch: %d\n",
           tuning->batch_size, tuning->transfer_size, tuning->prefetch_mode);

    mcf_result_t default_result = run_mcf_simulation(num_nodes, num_arcs, iterations);

    print_header("Running with MCF Profile");
    pgas_load_profile(&ctx, PGAS_PROFILE_MCF);

    tuning = pgas_get_default_tuning(PGAS_PROFILE_MCF);
    printf("  Batch: %zu, Transfer: %zu, Prefetch: %d (AGGRESSIVE)\n",
           tuning->batch_size, tuning->transfer_size, tuning->prefetch_mode);

    mcf_result_t mcf_result = run_mcf_simulation(num_nodes, num_arcs, iterations);

    print_header("MCF Benchmark Results");

    printf("\n  Phase Timing:\n");
    printf("  ┌─────────────────────┬────────────────┬────────────────┬────────────┐\n");
    printf("  │ Phase               │ DEFAULT        │ MCF Profile    │ Speedup    │\n");
    printf("  ├─────────────────────┼────────────────┼────────────────┼────────────┤\n");
    printf("  │ Reduced Cost        │ %8.3f sec   │ %8.3f sec   │ %+6.1f%%    │\n",
           default_result.reduced_cost_time, mcf_result.reduced_cost_time,
           (default_result.reduced_cost_time / mcf_result.reduced_cost_time - 1) * 100);
    printf("  │ Pivot Selection     │ %8.3f sec   │ %8.3f sec   │ %+6.1f%%    │\n",
           default_result.pivot_time, mcf_result.pivot_time,
           (default_result.pivot_time / mcf_result.pivot_time - 1) * 100);
    printf("  │ Tree Update         │ %8.3f sec   │ %8.3f sec   │ %+6.1f%%    │\n",
           default_result.update_time, mcf_result.update_time,
           (default_result.update_time / mcf_result.update_time - 1) * 100);
    printf("  └─────────────────────┴────────────────┴────────────────┴────────────┘\n");

    printf("\n  Performance Metrics:\n");
    printf("    Pivot operations:      %.2f M ops/sec (DEFAULT) vs %.2f M ops/sec (MCF)\n",
           default_result.pivot_ops_per_sec / 1e6, mcf_result.pivot_ops_per_sec / 1e6);
    printf("    Tree update ops:       %.2f M ops/sec (DEFAULT) vs %.2f M ops/sec (MCF)\n",
           default_result.tree_update_ops_per_sec / 1e6, mcf_result.tree_update_ops_per_sec / 1e6);
    printf("    Est. cache misses:     %.2f M\n", mcf_result.cache_misses_est / 1e6);

    double total_default = default_result.reduced_cost_time + default_result.pivot_time + default_result.update_time;
    double total_mcf = mcf_result.reduced_cost_time + mcf_result.pivot_time + mcf_result.update_time;
    double improvement = (total_default / total_mcf - 1) * 100;

    printf("\n  ► Overall MCF Profile Speedup: %+.1f%%\n", improvement);

    pgas_finalize(&ctx);

    printf("\n=== MCF Integration Test Complete ===\n");
    return 0;
}
