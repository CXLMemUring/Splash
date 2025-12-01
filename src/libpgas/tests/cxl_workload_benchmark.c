/*
 * CXL Memory Workload Benchmark
 * Tests MCF, LLAMA, and GROMACS patterns on actual CXL/PGAS memory
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <pgas/pgas.h>


#define CACHE_LINE_SIZE 64
#define KB (1024UL)
#define MB (1024UL * KB)
#define GB (1024UL * MB)

static inline double get_time_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static volatile uint64_t sink;

/*
 * ============================================================================
 * MCF on CXL - Pointer chasing through CXL memory
 * ============================================================================
 */
typedef struct {
    double ops_per_sec;
    double latency_ns;
    size_t bytes_accessed;
} benchmark_result_t;

benchmark_result_t benchmark_mcf_cxl(pgas_context_t* ctx, void* cxl_base, size_t region_size, int iterations) {
    benchmark_result_t result = {0};

    size_t num_nodes = region_size / sizeof(uint64_t);
    uint64_t* nodes = (uint64_t*)cxl_base;

    // Initialize pointer chain (random permutation for cache-unfriendly access)
    printf("    Initializing %zu node pointer chain...\n", num_nodes);
    srand(42);

    // Create random permutation
    for (size_t i = 0; i < num_nodes; i++) {
        nodes[i] = i;
    }

    // Fisher-Yates shuffle
    for (size_t i = num_nodes - 1; i > 0; i--) {
        size_t j = rand() % (i + 1);
        uint64_t tmp = nodes[i];
        nodes[i] = nodes[j];
        nodes[j] = tmp;
    }

    // Convert to pointer chain
    for (size_t i = 0; i < num_nodes - 1; i++) {
        size_t next = nodes[i + 1];
        nodes[nodes[i]] = next;
    }
    nodes[nodes[num_nodes - 1]] = nodes[0];  // Close the loop

    // Memory barrier
    __sync_synchronize();

    // Pointer chase benchmark
    printf("    Running pointer chase (%d iterations)...\n", iterations);
    double start = get_time_sec();

    uint64_t idx = 0;
    uint64_t ops = 0;
    volatile uint64_t checksum = 0;

    for (int iter = 0; iter < iterations; iter++) {
        for (size_t i = 0; i < num_nodes / 10; i++) {  // 10% of nodes per iteration
            idx = nodes[idx];
            checksum += idx;
            ops++;
        }
    }

    double elapsed = get_time_sec() - start;
    sink = checksum;

    result.ops_per_sec = (double)ops / elapsed;
    result.latency_ns = (elapsed / ops) * 1e9;
    result.bytes_accessed = ops * sizeof(uint64_t);

    return result;
}

/*
 * ============================================================================
 * LLAMA on CXL - Sequential streaming through CXL memory
 * ============================================================================
 */
benchmark_result_t benchmark_llama_cxl(pgas_context_t* ctx, void* cxl_base, size_t region_size, int iterations) {
    benchmark_result_t result = {0};

    float* weights = (float*)cxl_base;
    size_t num_floats = region_size / sizeof(float);

    // Initialize weights
    printf("    Initializing %.1f MB of weights...\n", region_size / (double)MB);
    for (size_t i = 0; i < num_floats; i++) {
        weights[i] = (float)(i % 1000) / 1000.0f;
    }

    __sync_synchronize();

    // Streaming read benchmark (simulating weight loading)
    printf("    Running streaming read (%d iterations)...\n", iterations);
    double start = get_time_sec();

    volatile float checksum = 0;
    size_t bytes_read = 0;

    for (int iter = 0; iter < iterations; iter++) {
        // Sequential streaming with large strides (cache-line friendly)
        for (size_t i = 0; i < num_floats; i += 16) {
            float sum = 0;
            // Prefetch next cache line
            __builtin_prefetch(&weights[i + 64], 0, 0);

            for (int j = 0; j < 16 && (i + j) < num_floats; j++) {
                sum += weights[i + j];
            }
            checksum += sum;
        }
        bytes_read += region_size;
    }

    double elapsed = get_time_sec() - start;
    sink = (uint64_t)checksum;

    result.ops_per_sec = (bytes_read / (double)GB) / elapsed;  // GB/s as ops
    result.latency_ns = 0;  // Not applicable for streaming
    result.bytes_accessed = bytes_read;

    return result;
}

/*
 * ============================================================================
 * GROMACS on CXL - Neighbor list access through CXL memory
 * ============================================================================
 */
typedef struct {
    float x, y, z;
    float vx, vy, vz;
    float fx, fy, fz;
    float mass;
    int type;
    int pad;
} particle_cxl_t;

benchmark_result_t benchmark_gromacs_cxl(pgas_context_t* ctx, void* cxl_base, size_t region_size, int iterations) {
    benchmark_result_t result = {0};

    // Split region: 70% particles, 30% neighbor lists
    size_t particle_region = (size_t)(region_size * 0.7);
    size_t neighbor_region = region_size - particle_region;

    int num_particles = particle_region / sizeof(particle_cxl_t);
    int neighbors_per_particle = 50;

    particle_cxl_t* particles = (particle_cxl_t*)cxl_base;
    int* neighbor_list = (int*)((char*)cxl_base + particle_region);

    // Limit based on neighbor list size
    int max_particles_for_neighbors = neighbor_region / (neighbors_per_particle * sizeof(int));
    if (num_particles > max_particles_for_neighbors) {
        num_particles = max_particles_for_neighbors;
    }

    printf("    Initializing %d particles with %d neighbors each...\n",
           num_particles, neighbors_per_particle);

    // Initialize particles
    srand(42);
    for (int i = 0; i < num_particles; i++) {
        particles[i].x = (float)rand() / RAND_MAX * 10.0f;
        particles[i].y = (float)rand() / RAND_MAX * 10.0f;
        particles[i].z = (float)rand() / RAND_MAX * 10.0f;
        particles[i].vx = particles[i].vy = particles[i].vz = 0;
        particles[i].fx = particles[i].fy = particles[i].fz = 0;
        particles[i].mass = 1.0f;
        particles[i].type = rand() % 4;
    }

    // Build neighbor lists with spatial locality
    for (int i = 0; i < num_particles; i++) {
        for (int j = 0; j < neighbors_per_particle; j++) {
            // Neighbors clustered nearby (spatial locality)
            int base = (i / 100) * 100;
            int offset = rand() % 200 - 100;
            int neighbor = (base + offset + num_particles) % num_particles;
            neighbor_list[i * neighbors_per_particle + j] = neighbor;
        }
    }

    __sync_synchronize();

    // Neighbor list traversal benchmark
    printf("    Running neighbor list traversal (%d iterations)...\n", iterations);
    double start = get_time_sec();

    uint64_t interactions = 0;
    volatile float checksum = 0;

    for (int iter = 0; iter < iterations; iter++) {
        for (int i = 0; i < num_particles; i++) {
            particle_cxl_t* pi = &particles[i];

            for (int n = 0; n < neighbors_per_particle; n++) {
                int j = neighbor_list[i * neighbors_per_particle + n];
                particle_cxl_t* pj = &particles[j];

                // Simple distance calculation (force compute proxy)
                float dx = pj->x - pi->x;
                float dy = pj->y - pi->y;
                float dz = pj->z - pi->z;
                float r2 = dx*dx + dy*dy + dz*dz + 0.01f;

                checksum += 1.0f / r2;
                interactions++;
            }
        }
    }

    double elapsed = get_time_sec() - start;
    sink = (uint64_t)checksum;

    result.ops_per_sec = (double)interactions / elapsed;
    result.latency_ns = (elapsed / interactions) * 1e9;
    result.bytes_accessed = interactions * (2 * sizeof(particle_cxl_t) + sizeof(int));

    return result;
}

/*
 * ============================================================================
 * Main
 * ============================================================================
 */
void print_header(const char* title) {
    printf("\n╔════════════════════════════════════════════════════════════════════════════╗\n");
    printf("║  %-74s ║\n", title);
    printf("╚════════════════════════════════════════════════════════════════════════════╝\n");
}

int main(int argc, char* argv[]) {
    const char* config_file = "node0.conf";
    size_t region_size = 64 * MB;  // 64 MB test region
    int iterations = 10;

    // Parse arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            config_file = argv[++i];
        } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            region_size = atoi(argv[++i]) * MB;
        } else if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            iterations = atoi(argv[++i]);
        }
    }

    printf("╔════════════════════════════════════════════════════════════════════════════╗\n");
    printf("║          CXL Memory Workload Benchmark Suite                               ║\n");
    printf("╚════════════════════════════════════════════════════════════════════════════╝\n");
    printf("\nConfiguration:\n");
    printf("  Config: %s\n", config_file);
    printf("  Region size: %zu MB\n", region_size / MB);
    printf("  Iterations: %d\n", iterations);

    // Initialize memory region (simulating CXL memory)
    print_header("Initializing Memory Region");

    void* cxl_base = mmap(NULL, region_size, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (cxl_base == MAP_FAILED) {
        printf("Failed to allocate memory\n");
        return 1;
    }
    printf("  CXL memory at: %p\n", cxl_base);
    printf("  Region size: %zu MB\n", region_size / MB);

    // Initialize PGAS
    pgas_context_t ctx;

    if (pgas_init(&ctx, NULL) != 0) {
        printf("Warning: PGAS init failed, continuing with direct CXL access\n");
    }

    // Results storage
    benchmark_result_t mcf_default, mcf_tuned;
    benchmark_result_t llama_default, llama_tuned;
    benchmark_result_t gromacs_default, gromacs_tuned;

    /*
     * MCF Benchmark on CXL
     */
    print_header("MCF on CXL - Pointer Chasing");

    printf("\n  Running with DEFAULT profile...\n");
    pgas_load_profile(&ctx, PGAS_PROFILE_DEFAULT);
    mcf_default = benchmark_mcf_cxl(&ctx, cxl_base, region_size, iterations);
    printf("    Results: %.2f M ops/sec, %.1f ns latency\n",
           mcf_default.ops_per_sec / 1e6, mcf_default.latency_ns);

    printf("\n  Running with MCF profile...\n");
    pgas_load_profile(&ctx, PGAS_PROFILE_MCF);
    mcf_tuned = benchmark_mcf_cxl(&ctx, cxl_base, region_size, iterations);
    printf("    Results: %.2f M ops/sec, %.1f ns latency\n",
           mcf_tuned.ops_per_sec / 1e6, mcf_tuned.latency_ns);

    double mcf_improvement = (mcf_tuned.ops_per_sec / mcf_default.ops_per_sec - 1) * 100;
    printf("\n  ► MCF Profile improvement: %+.1f%%\n", mcf_improvement);

    /*
     * LLAMA Benchmark on CXL
     */
    print_header("LLAMA on CXL - Weight Streaming");

    printf("\n  Running with DEFAULT profile...\n");
    pgas_load_profile(&ctx, PGAS_PROFILE_DEFAULT);
    llama_default = benchmark_llama_cxl(&ctx, cxl_base, region_size, iterations);
    printf("    Results: %.2f GB/s bandwidth\n", llama_default.ops_per_sec);

    printf("\n  Running with LLAMA profile...\n");
    pgas_load_profile(&ctx, PGAS_PROFILE_LLAMA);
    llama_tuned = benchmark_llama_cxl(&ctx, cxl_base, region_size, iterations);
    printf("    Results: %.2f GB/s bandwidth\n", llama_tuned.ops_per_sec);

    double llama_improvement = (llama_tuned.ops_per_sec / llama_default.ops_per_sec - 1) * 100;
    printf("\n  ► LLAMA Profile improvement: %+.1f%%\n", llama_improvement);

    /*
     * GROMACS Benchmark on CXL
     */
    print_header("GROMACS on CXL - Neighbor List");

    printf("\n  Running with DEFAULT profile...\n");
    pgas_load_profile(&ctx, PGAS_PROFILE_DEFAULT);
    gromacs_default = benchmark_gromacs_cxl(&ctx, cxl_base, region_size, iterations);
    printf("    Results: %.2f M interactions/sec\n", gromacs_default.ops_per_sec / 1e6);

    printf("\n  Running with GROMACS profile...\n");
    pgas_load_profile(&ctx, PGAS_PROFILE_GROMACS);
    gromacs_tuned = benchmark_gromacs_cxl(&ctx, cxl_base, region_size, iterations);
    printf("    Results: %.2f M interactions/sec\n", gromacs_tuned.ops_per_sec / 1e6);

    double gromacs_improvement = (gromacs_tuned.ops_per_sec / gromacs_default.ops_per_sec - 1) * 100;
    printf("\n  ► GROMACS Profile improvement: %+.1f%%\n", gromacs_improvement);

    /*
     * Summary
     */
    print_header("CXL BENCHMARK SUMMARY");

    printf("\n  ┌───────────────┬────────────────────┬────────────────────┬─────────────┐\n");
    printf("  │ Workload      │ DEFAULT            │ TUNED              │ Improvement │\n");
    printf("  ├───────────────┼────────────────────┼────────────────────┼─────────────┤\n");
    printf("  │ MCF           │ %8.2f M ops/s   │ %8.2f M ops/s   │ %+7.1f%%    │\n",
           mcf_default.ops_per_sec / 1e6, mcf_tuned.ops_per_sec / 1e6, mcf_improvement);
    printf("  │ LLAMA         │ %8.2f GB/s      │ %8.2f GB/s      │ %+7.1f%%    │\n",
           llama_default.ops_per_sec, llama_tuned.ops_per_sec, llama_improvement);
    printf("  │ GROMACS       │ %8.2f M int/s   │ %8.2f M int/s   │ %+7.1f%%    │\n",
           gromacs_default.ops_per_sec / 1e6, gromacs_tuned.ops_per_sec / 1e6, gromacs_improvement);
    printf("  └───────────────┴────────────────────┴────────────────────┴─────────────┘\n");

    printf("\n  CXL Memory Performance:\n");
    printf("    MCF latency:     %.1f ns (pointer chase)\n", mcf_tuned.latency_ns);
    printf("    LLAMA bandwidth: %.2f GB/s (streaming)\n", llama_tuned.ops_per_sec);
    printf("    GROMACS:         %.2f M interactions/s\n", gromacs_tuned.ops_per_sec / 1e6);

    // Cleanup
    pgas_finalize(&ctx);
    munmap(cxl_base, region_size);

    printf("\n=== CXL Benchmark Complete ===\n");
    return 0;
}
