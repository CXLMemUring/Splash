/*
 * PGAS Profile Benchmark
 *
 * Tests different tuning profiles with workload-representative access patterns:
 * - MCF: Pointer-chasing (linked list traversal)
 * - LLAMA: Sequential streaming (large sequential reads)
 * - GROMACS: Neighbor-list pattern (batch reads with locality)
 * - GRAPH: Random access (irregular vertex access)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pgas/pgas.h>

#define ARRAY_SIZE (1024 * 1024)  // 1M elements
#define NUM_ITERATIONS 1000
#define POINTER_CHASE_DEPTH 10000
#define STREAMING_CHUNK_SIZE (64 * 1024)  // 64KB chunks
#define NEIGHBOR_LIST_SIZE 100
#define RANDOM_ACCESS_COUNT 10000

// Timer utilities
static inline double get_time_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

typedef struct {
    double elapsed_sec;
    double ops_per_sec;
    double bandwidth_mbps;
    uint64_t remote_reads;
    uint64_t bytes_transferred;
} benchmark_result_t;

/*
 * MCF-style benchmark: Pointer chasing
 * Simulates linked list traversal with indirect memory access
 */
benchmark_result_t benchmark_pointer_chase(pgas_context_t* ctx, int is_remote) {
    benchmark_result_t result = {0};

    // Allocate array of "next" pointers (indices)
    uint64_t* next_array = malloc(POINTER_CHASE_DEPTH * sizeof(uint64_t));

    // Create random permutation for pointer chase
    for (int i = 0; i < POINTER_CHASE_DEPTH; i++) {
        next_array[i] = i;
    }
    // Fisher-Yates shuffle
    for (int i = POINTER_CHASE_DEPTH - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        uint64_t tmp = next_array[i];
        next_array[i] = next_array[j];
        next_array[j] = tmp;
    }

    // Allocate PGAS memory
    uint16_t target_node = is_remote ? (ctx->local_node_id == 0 ? 1 : 0) : ctx->local_node_id;
    pgas_ptr_t ptr = pgas_alloc_on_node(ctx, POINTER_CHASE_DEPTH * sizeof(uint64_t), target_node);

    if (pgas_ptr_is_null(ptr)) {
        printf("  Failed to allocate PGAS memory\n");
        free(next_array);
        return result;
    }

    // Write the array
    pgas_put(ctx, ptr, next_array, POINTER_CHASE_DEPTH * sizeof(uint64_t));
    pgas_barrier(ctx);

    // Reset stats
    pgas_reset_stats(ctx);

    // Pointer chase benchmark
    double start = get_time_sec();
    uint64_t index = 0;
    uint64_t checksum = 0;

    for (int iter = 0; iter < NUM_ITERATIONS; iter++) {
        index = 0;
        for (int i = 0; i < POINTER_CHASE_DEPTH; i++) {
            uint64_t next;
            pgas_ptr_t elem_ptr = pgas_ptr_add(ptr, index * sizeof(uint64_t));
            pgas_get(ctx, &next, elem_ptr, sizeof(uint64_t));
            checksum += next;
            index = next % POINTER_CHASE_DEPTH;
        }
    }

    double end = get_time_sec();

    // Get stats
    pgas_stats_t stats;
    pgas_get_stats(ctx, &stats);

    result.elapsed_sec = end - start;
    result.ops_per_sec = (double)(NUM_ITERATIONS * POINTER_CHASE_DEPTH) / result.elapsed_sec;
    result.bandwidth_mbps = (stats.bytes_transferred / 1e6) / result.elapsed_sec;
    result.remote_reads = stats.remote_reads;
    result.bytes_transferred = stats.bytes_transferred;

    // Cleanup
    pgas_free(ctx, ptr);
    free(next_array);

    // Use checksum to prevent optimization
    if (checksum == 0) printf("");

    return result;
}

/*
 * LLAMA-style benchmark: Sequential streaming
 * Simulates loading model weights sequentially
 */
benchmark_result_t benchmark_streaming(pgas_context_t* ctx, int is_remote) {
    benchmark_result_t result = {0};

    size_t total_size = ARRAY_SIZE * sizeof(uint64_t);  // 8MB
    uint64_t* local_buf = malloc(STREAMING_CHUNK_SIZE);

    // Allocate PGAS memory
    uint16_t target_node = is_remote ? (ctx->local_node_id == 0 ? 1 : 0) : ctx->local_node_id;
    pgas_ptr_t ptr = pgas_alloc_on_node(ctx, total_size, target_node);

    if (pgas_ptr_is_null(ptr)) {
        printf("  Failed to allocate PGAS memory\n");
        free(local_buf);
        return result;
    }

    // Initialize with pattern
    uint64_t* init_data = malloc(total_size);
    for (size_t i = 0; i < ARRAY_SIZE; i++) {
        init_data[i] = i;
    }
    pgas_put(ctx, ptr, init_data, total_size);
    pgas_barrier(ctx);
    free(init_data);

    // Reset stats
    pgas_reset_stats(ctx);

    // Sequential streaming benchmark
    double start = get_time_sec();
    uint64_t checksum = 0;

    int num_chunks = total_size / STREAMING_CHUNK_SIZE;
    for (int iter = 0; iter < 100; iter++) {  // 100 iterations
        for (int chunk = 0; chunk < num_chunks; chunk++) {
            pgas_ptr_t chunk_ptr = pgas_ptr_add(ptr, chunk * STREAMING_CHUNK_SIZE);
            pgas_get(ctx, local_buf, chunk_ptr, STREAMING_CHUNK_SIZE);

            // Touch data
            for (size_t i = 0; i < STREAMING_CHUNK_SIZE / sizeof(uint64_t); i++) {
                checksum += local_buf[i];
            }
        }
    }

    double end = get_time_sec();

    // Get stats
    pgas_stats_t stats;
    pgas_get_stats(ctx, &stats);

    result.elapsed_sec = end - start;
    result.ops_per_sec = (double)(100 * num_chunks) / result.elapsed_sec;
    result.bandwidth_mbps = (stats.bytes_transferred / 1e6) / result.elapsed_sec;
    result.remote_reads = stats.remote_reads;
    result.bytes_transferred = stats.bytes_transferred;

    // Cleanup
    pgas_free(ctx, ptr);
    free(local_buf);

    if (checksum == 0) printf("");

    return result;
}

/*
 * GROMACS-style benchmark: Neighbor-list access
 * Simulates accessing particle data based on neighbor lists
 */
benchmark_result_t benchmark_neighbor_list(pgas_context_t* ctx, int is_remote) {
    benchmark_result_t result = {0};

    // Particle data: position (x,y,z) + force (fx,fy,fz) = 6 doubles = 48 bytes
    size_t particle_size = 48;
    int num_particles = 10000;
    size_t total_size = num_particles * particle_size;

    // Create neighbor lists (each particle has ~100 neighbors)
    int** neighbor_lists = malloc(num_particles * sizeof(int*));
    int* neighbor_counts = malloc(num_particles * sizeof(int));

    for (int i = 0; i < num_particles; i++) {
        neighbor_counts[i] = 50 + rand() % 100;  // 50-150 neighbors
        neighbor_lists[i] = malloc(neighbor_counts[i] * sizeof(int));
        for (int j = 0; j < neighbor_counts[i]; j++) {
            neighbor_lists[i][j] = rand() % num_particles;
        }
    }

    // Allocate PGAS memory
    uint16_t target_node = is_remote ? (ctx->local_node_id == 0 ? 1 : 0) : ctx->local_node_id;
    pgas_ptr_t ptr = pgas_alloc_on_node(ctx, total_size, target_node);

    if (pgas_ptr_is_null(ptr)) {
        printf("  Failed to allocate PGAS memory\n");
        for (int i = 0; i < num_particles; i++) free(neighbor_lists[i]);
        free(neighbor_lists);
        free(neighbor_counts);
        return result;
    }

    // Initialize particle data
    double* particles = malloc(total_size);
    for (int i = 0; i < num_particles * 6; i++) {
        particles[i] = (double)rand() / RAND_MAX;
    }
    pgas_put(ctx, ptr, particles, total_size);
    pgas_barrier(ctx);
    free(particles);

    // Reset stats
    pgas_reset_stats(ctx);

    // Neighbor-list benchmark
    double start = get_time_sec();
    double particle_buf[6];
    double checksum = 0;

    for (int iter = 0; iter < 10; iter++) {  // 10 MD steps
        for (int i = 0; i < num_particles; i++) {
            // Access neighbors
            for (int j = 0; j < neighbor_counts[i]; j++) {
                int neighbor = neighbor_lists[i][j];
                pgas_ptr_t neigh_ptr = pgas_ptr_add(ptr, neighbor * particle_size);
                pgas_get(ctx, particle_buf, neigh_ptr, particle_size);
                checksum += particle_buf[0] + particle_buf[1] + particle_buf[2];
            }
        }
    }

    double end = get_time_sec();

    // Get stats
    pgas_stats_t stats;
    pgas_get_stats(ctx, &stats);

    // Count total neighbor accesses
    long total_accesses = 0;
    for (int i = 0; i < num_particles; i++) {
        total_accesses += neighbor_counts[i];
    }

    result.elapsed_sec = end - start;
    result.ops_per_sec = (double)(10 * total_accesses) / result.elapsed_sec;
    result.bandwidth_mbps = (stats.bytes_transferred / 1e6) / result.elapsed_sec;
    result.remote_reads = stats.remote_reads;
    result.bytes_transferred = stats.bytes_transferred;

    // Cleanup
    pgas_free(ctx, ptr);
    for (int i = 0; i < num_particles; i++) free(neighbor_lists[i]);
    free(neighbor_lists);
    free(neighbor_counts);

    if (checksum == 0) printf("");

    return result;
}

/*
 * Graph-style benchmark: Random access
 * Simulates BFS-style irregular vertex access
 */
benchmark_result_t benchmark_random_access(pgas_context_t* ctx, int is_remote) {
    benchmark_result_t result = {0};

    // Vertex data: 64 bytes per vertex
    size_t vertex_size = 64;
    int num_vertices = 100000;
    size_t total_size = num_vertices * vertex_size;

    // Pre-generate random access pattern
    int* access_pattern = malloc(RANDOM_ACCESS_COUNT * sizeof(int));
    for (int i = 0; i < RANDOM_ACCESS_COUNT; i++) {
        access_pattern[i] = rand() % num_vertices;
    }

    // Allocate PGAS memory
    uint16_t target_node = is_remote ? (ctx->local_node_id == 0 ? 1 : 0) : ctx->local_node_id;
    pgas_ptr_t ptr = pgas_alloc_on_node(ctx, total_size, target_node);

    if (pgas_ptr_is_null(ptr)) {
        printf("  Failed to allocate PGAS memory\n");
        free(access_pattern);
        return result;
    }

    // Initialize vertex data
    char* vertices = malloc(total_size);
    memset(vertices, 0xAB, total_size);
    pgas_put(ctx, ptr, vertices, total_size);
    pgas_barrier(ctx);
    free(vertices);

    // Reset stats
    pgas_reset_stats(ctx);

    // Random access benchmark
    double start = get_time_sec();
    char vertex_buf[64];
    uint64_t checksum = 0;

    for (int iter = 0; iter < 100; iter++) {
        for (int i = 0; i < RANDOM_ACCESS_COUNT; i++) {
            int vertex = access_pattern[i];
            pgas_ptr_t vert_ptr = pgas_ptr_add(ptr, vertex * vertex_size);
            pgas_get(ctx, vertex_buf, vert_ptr, vertex_size);
            checksum += vertex_buf[0];
        }
    }

    double end = get_time_sec();

    // Get stats
    pgas_stats_t stats;
    pgas_get_stats(ctx, &stats);

    result.elapsed_sec = end - start;
    result.ops_per_sec = (double)(100 * RANDOM_ACCESS_COUNT) / result.elapsed_sec;
    result.bandwidth_mbps = (stats.bytes_transferred / 1e6) / result.elapsed_sec;
    result.remote_reads = stats.remote_reads;
    result.bytes_transferred = stats.bytes_transferred;

    // Cleanup
    pgas_free(ctx, ptr);
    free(access_pattern);

    if (checksum == 0) printf("");

    return result;
}

void print_result(const char* name, benchmark_result_t* result) {
    printf("  %-20s %8.3f sec  %12.0f ops/s  %8.2f MB/s  %lu reads\n",
           name, result->elapsed_sec, result->ops_per_sec,
           result->bandwidth_mbps, result->remote_reads);
}

void run_benchmark_suite(pgas_context_t* ctx, const char* profile_name, pgas_profile_t profile) {
    printf("\n");
    printf("╔════════════════════════════════════════════════════════════════╗\n");
    printf("║  Benchmark: %-50s ║\n", profile_name);
    printf("╚════════════════════════════════════════════════════════════════╝\n");

    // Load profile
    pgas_load_profile(ctx, profile);

    printf("\nRunning benchmarks (local memory)...\n");

    benchmark_result_t ptr_chase = benchmark_pointer_chase(ctx, 0);
    print_result("Pointer Chase", &ptr_chase);

    benchmark_result_t streaming = benchmark_streaming(ctx, 0);
    print_result("Streaming", &streaming);

    benchmark_result_t neighbor = benchmark_neighbor_list(ctx, 0);
    print_result("Neighbor List", &neighbor);

    benchmark_result_t random_acc = benchmark_random_access(ctx, 0);
    print_result("Random Access", &random_acc);

    // If we have 2 nodes, also test remote
    if (ctx->num_nodes > 1) {
        printf("\nRunning benchmarks (remote memory via CXL)...\n");

        ptr_chase = benchmark_pointer_chase(ctx, 1);
        print_result("Pointer Chase", &ptr_chase);

        streaming = benchmark_streaming(ctx, 1);
        print_result("Streaming", &streaming);

        neighbor = benchmark_neighbor_list(ctx, 1);
        print_result("Neighbor List", &neighbor);

        random_acc = benchmark_random_access(ctx, 1);
        print_result("Random Access", &random_acc);
    }
}

int main(int argc, char* argv[]) {
    const char* config_file = NULL;

    // Parse args
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            config_file = argv[++i];
        }
    }

    if (!config_file) {
        printf("Usage: %s -c <config_file>\n", argv[0]);
        return 1;
    }

    // Initialize PGAS
    pgas_context_t ctx;
    if (pgas_init(&ctx, config_file) != 0) {
        fprintf(stderr, "Failed to initialize PGAS\n");
        return 1;
    }

    printf("╔════════════════════════════════════════════════════════════════╗\n");
    printf("║           PGAS Profile Benchmark Suite                         ║\n");
    printf("╚════════════════════════════════════════════════════════════════╝\n");
    printf("\nNode %d of %d initialized\n", ctx.local_node_id, ctx.num_nodes);

    // Seed random
    srand(42);  // Fixed seed for reproducibility

    // Run benchmarks with each profile
    run_benchmark_suite(&ctx, "DEFAULT Profile", PGAS_PROFILE_DEFAULT);
    run_benchmark_suite(&ctx, "MCF Profile (Pointer Chasing)", PGAS_PROFILE_MCF);
    run_benchmark_suite(&ctx, "LLAMA Profile (Streaming)", PGAS_PROFILE_LLAMA);
    run_benchmark_suite(&ctx, "GROMACS Profile (Neighbor List)", PGAS_PROFILE_GROMACS);
    run_benchmark_suite(&ctx, "GRAPH Profile (Random Access)", PGAS_PROFILE_GRAPH);

    // Cleanup
    pgas_finalize(&ctx);

    printf("\n=== Benchmark Complete ===\n");
    return 0;
}
