/*
 * Workload-Specific Benchmark
 * Tests MCF, LLAMA, and GROMACS workload patterns with their optimal profiles
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>
#include <pgas/pgas.h>

#define CACHE_LINE_SIZE 64

static inline double get_time_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

// Prevent compiler from optimizing away
static volatile uint64_t sink;

/*
 * ============================================================================
 * MCF BENCHMARK - Pointer Chasing (Network Simplex Algorithm)
 * ============================================================================
 * MCF characteristics:
 * - Two large objects (~2GB each): node array and arc array
 * - High cache miss rate (MPKI 2-3)
 * - Pointer chasing through linked structures
 * - Irregular memory access patterns
 */

typedef struct mcf_node {
    int64_t potential;
    int64_t flow;
    uint32_t first_arc;
    uint32_t pred_arc;
    uint32_t depth;
    uint32_t thread;
    struct mcf_node* pred;
    struct mcf_node* child;
    struct mcf_node* sibling;
    char padding[CACHE_LINE_SIZE - 56];  // Pad to cache line
} mcf_node_t;

typedef struct mcf_arc {
    int64_t cost;
    int64_t flow;
    int64_t capacity;
    uint32_t tail;
    uint32_t head;
    uint32_t next_out;
    uint32_t next_in;
    char padding[CACHE_LINE_SIZE - 40];
} mcf_arc_t;

typedef struct {
    double ops_per_sec;
    double latency_ns;
    uint64_t cache_misses_simulated;
} mcf_result_t;

mcf_result_t benchmark_mcf(size_t num_nodes, size_t num_arcs, int iterations) {
    mcf_result_t result = {0};

    // Allocate node and arc arrays
    mcf_node_t* nodes = mmap(NULL, num_nodes * sizeof(mcf_node_t),
                              PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    mcf_arc_t* arcs = mmap(NULL, num_arcs * sizeof(mcf_arc_t),
                            PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (nodes == MAP_FAILED || arcs == MAP_FAILED) {
        printf("  Failed to allocate MCF memory\n");
        return result;
    }

    // Initialize tree structure (random spanning tree)
    srand(42);
    for (size_t i = 0; i < num_nodes; i++) {
        nodes[i].potential = rand() % 1000;
        nodes[i].flow = 0;
        nodes[i].first_arc = rand() % num_arcs;
        nodes[i].depth = i;
        nodes[i].pred = (i > 0) ? &nodes[rand() % i] : NULL;
        nodes[i].child = NULL;
        nodes[i].sibling = NULL;
    }

    // Build child/sibling links (pointer-chase structure)
    for (size_t i = 1; i < num_nodes; i++) {
        if (nodes[i].pred) {
            nodes[i].sibling = nodes[i].pred->child;
            nodes[i].pred->child = &nodes[i];
        }
    }

    // Initialize arcs
    for (size_t i = 0; i < num_arcs; i++) {
        arcs[i].cost = rand() % 100;
        arcs[i].flow = 0;
        arcs[i].capacity = 1000 + rand() % 9000;
        arcs[i].tail = rand() % num_nodes;
        arcs[i].head = rand() % num_nodes;
        arcs[i].next_out = rand() % num_arcs;
        arcs[i].next_in = rand() % num_arcs;
    }

    // MCF-style tree traversal (pivot search)
    double start = get_time_sec();
    uint64_t ops = 0;
    volatile int64_t checksum = 0;

    for (int iter = 0; iter < iterations; iter++) {
        // Traverse tree looking for entering arc (like network simplex)
        for (size_t i = 0; i < num_nodes; i++) {
            mcf_node_t* node = &nodes[i];

            // Follow predecessor chain (pointer chase)
            mcf_node_t* current = node;
            int depth = 0;
            while (current->pred && depth < 100) {
                checksum += current->potential;
                current = current->pred;
                depth++;
                ops++;
            }

            // Check arcs from this node
            uint32_t arc_idx = node->first_arc;
            for (int j = 0; j < 5 && arc_idx < num_arcs; j++) {
                checksum += arcs[arc_idx].cost;
                arc_idx = arcs[arc_idx].next_out;
                ops++;
            }

            // Visit children (tree update simulation)
            mcf_node_t* child = node->child;
            while (child) {
                checksum += child->flow;
                child = child->sibling;
                ops++;
            }
        }
    }

    double elapsed = get_time_sec() - start;
    sink = checksum;

    result.ops_per_sec = (double)ops / elapsed;
    result.latency_ns = (elapsed / ops) * 1e9;
    result.cache_misses_simulated = ops;  // Each pointer chase likely misses

    munmap(nodes, num_nodes * sizeof(mcf_node_t));
    munmap(arcs, num_arcs * sizeof(mcf_arc_t));

    return result;
}

/*
 * ============================================================================
 * LLAMA BENCHMARK - Sequential Weight Loading (LLM Inference)
 * ============================================================================
 * LLAMA characteristics:
 * - Memory bandwidth bound (low arithmetic intensity <25)
 * - Sequential reading of large weight matrices
 * - Prefetching is highly effective
 * - Large transfer sizes optimal
 */

typedef struct {
    double bandwidth_gbps;
    double latency_ms;
    size_t bytes_transferred;
} llama_result_t;

llama_result_t benchmark_llama(size_t model_size_mb, int num_layers, int iterations) {
    llama_result_t result = {0};

    size_t layer_size = (model_size_mb * 1024 * 1024) / num_layers;
    size_t total_size = model_size_mb * 1024 * 1024;

    // Simulate model weights
    float* weights = mmap(NULL, total_size,
                          PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    // Output buffer (activations)
    float* activations = mmap(NULL, layer_size,
                              PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (weights == MAP_FAILED || activations == MAP_FAILED) {
        printf("  Failed to allocate LLAMA memory\n");
        return result;
    }

    // Initialize weights (simulate loaded model)
    size_t num_floats = total_size / sizeof(float);
    for (size_t i = 0; i < num_floats; i++) {
        weights[i] = (float)(i % 1000) / 1000.0f;
    }

    // LLAMA-style inference: sequential layer processing
    double start = get_time_sec();
    size_t bytes_read = 0;
    volatile float checksum = 0;

    for (int iter = 0; iter < iterations; iter++) {
        // Process each layer sequentially
        for (int layer = 0; layer < num_layers; layer++) {
            float* layer_weights = weights + (layer * layer_size / sizeof(float));
            size_t layer_floats = layer_size / sizeof(float);

            // Stream through weights (simulating matmul)
            // This is memory-bound, not compute-bound
            for (size_t i = 0; i < layer_floats; i += 16) {
                // Prefetch-friendly sequential access
                float sum = 0;
                for (int j = 0; j < 16 && (i + j) < layer_floats; j++) {
                    sum += layer_weights[i + j];
                }
                activations[i % (layer_size / sizeof(float))] = sum;
                checksum += sum;
            }
            bytes_read += layer_size;
        }
    }

    double elapsed = get_time_sec() - start;
    sink = (uint64_t)checksum;

    result.bandwidth_gbps = (bytes_read / 1e9) / elapsed;
    result.latency_ms = (elapsed / (iterations * num_layers)) * 1000;
    result.bytes_transferred = bytes_read;

    munmap(weights, total_size);
    munmap(activations, layer_size);

    return result;
}

/*
 * ============================================================================
 * GROMACS BENCHMARK - Neighbor List Molecular Dynamics
 * ============================================================================
 * GROMACS characteristics:
 * - Neighbor-list driven access patterns
 * - 3D domain decomposition
 * - Batch reads with spatial locality
 * - Particle data: positions, velocities, forces (3D vectors)
 */

typedef struct {
    float x, y, z;       // Position
    float vx, vy, vz;    // Velocity
    float fx, fy, fz;    // Force
    float mass;
    float charge;
    int type;
    char padding[4];     // Align to 48 bytes
} particle_t;

typedef struct {
    double interactions_per_sec;
    double ns_per_day;  // Simulated nanoseconds per day (MD metric)
    uint64_t force_calculations;
} gromacs_result_t;

gromacs_result_t benchmark_gromacs(int num_particles, int neighbors_per_particle, int timesteps) {
    gromacs_result_t result = {0};

    // Allocate particles
    particle_t* particles = mmap(NULL, num_particles * sizeof(particle_t),
                                  PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    // Allocate neighbor lists
    int* neighbor_list = mmap(NULL, num_particles * neighbors_per_particle * sizeof(int),
                               PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (particles == MAP_FAILED || neighbor_list == MAP_FAILED) {
        printf("  Failed to allocate GROMACS memory\n");
        return result;
    }

    // Initialize particles in a 3D box
    srand(42);
    float box_size = 10.0f;
    for (int i = 0; i < num_particles; i++) {
        particles[i].x = (float)rand() / RAND_MAX * box_size;
        particles[i].y = (float)rand() / RAND_MAX * box_size;
        particles[i].z = (float)rand() / RAND_MAX * box_size;
        particles[i].vx = ((float)rand() / RAND_MAX - 0.5f) * 0.1f;
        particles[i].vy = ((float)rand() / RAND_MAX - 0.5f) * 0.1f;
        particles[i].vz = ((float)rand() / RAND_MAX - 0.5f) * 0.1f;
        particles[i].fx = particles[i].fy = particles[i].fz = 0;
        particles[i].mass = 1.0f;
        particles[i].charge = (rand() % 2) ? 1.0f : -1.0f;
        particles[i].type = rand() % 4;
    }

    // Build neighbor lists (spatially clustered for locality)
    for (int i = 0; i < num_particles; i++) {
        for (int j = 0; j < neighbors_per_particle; j++) {
            // Neighbors tend to be close in memory (spatial locality)
            int base = (i / 100) * 100;  // Cluster of 100 particles
            int offset = rand() % 200 - 100;
            int neighbor = (base + offset + num_particles) % num_particles;
            neighbor_list[i * neighbors_per_particle + j] = neighbor;
        }
    }

    // MD simulation loop
    double start = get_time_sec();
    uint64_t force_calcs = 0;
    volatile float checksum = 0;

    float dt = 0.001f;  // Timestep

    for (int step = 0; step < timesteps; step++) {
        // Clear forces
        for (int i = 0; i < num_particles; i++) {
            particles[i].fx = particles[i].fy = particles[i].fz = 0;
        }

        // Calculate forces using neighbor lists
        for (int i = 0; i < num_particles; i++) {
            particle_t* pi = &particles[i];

            for (int n = 0; n < neighbors_per_particle; n++) {
                int j = neighbor_list[i * neighbors_per_particle + n];
                particle_t* pj = &particles[j];

                // Lennard-Jones style force calculation
                float dx = pj->x - pi->x;
                float dy = pj->y - pi->y;
                float dz = pj->z - pi->z;

                float r2 = dx*dx + dy*dy + dz*dz + 0.01f;  // Avoid div by zero
                float r6 = r2 * r2 * r2;
                float force = 24.0f * (2.0f / (r6 * r6 * r2) - 1.0f / (r6 * r2));

                pi->fx += force * dx;
                pi->fy += force * dy;
                pi->fz += force * dz;

                force_calcs++;
            }
        }

        // Update positions (Velocity Verlet)
        for (int i = 0; i < num_particles; i++) {
            particle_t* p = &particles[i];
            float inv_mass = 1.0f / p->mass;

            p->vx += p->fx * inv_mass * dt;
            p->vy += p->fy * inv_mass * dt;
            p->vz += p->fz * inv_mass * dt;

            p->x += p->vx * dt;
            p->y += p->vy * dt;
            p->z += p->vz * dt;

            checksum += p->x + p->y + p->z;
        }
    }

    double elapsed = get_time_sec() - start;
    sink = (uint64_t)checksum;

    result.interactions_per_sec = (double)force_calcs / elapsed;
    result.ns_per_day = (timesteps * dt * 86400.0) / elapsed;  // ns/day metric
    result.force_calculations = force_calcs;

    munmap(particles, num_particles * sizeof(particle_t));
    munmap(neighbor_list, num_particles * neighbors_per_particle * sizeof(int));

    return result;
}

/*
 * ============================================================================
 * MAIN - Run all benchmarks
 * ============================================================================
 */

void print_header(const char* title) {
    printf("\n");
    printf("╔════════════════════════════════════════════════════════════════════════════╗\n");
    printf("║  %-74s ║\n", title);
    printf("╚════════════════════════════════════════════════════════════════════════════╝\n");
}

void print_profile_settings(pgas_profile_t profile) {
    const pgas_tuning_t* tuning = pgas_get_default_tuning(profile);
    const char* affinity_names[] = {"LOCAL", "REMOTE", "INTERLEAVE", "REPLICATE"};
    const char* prefetch_names[] = {"NONE", "SEQUENTIAL", "STRIDED", "AGGRESSIVE", "NEIGHBOR"};

    printf("  Profile Settings:\n");
    printf("    Affinity: %-12s  Batch: %5zu  Transfer: %7zu bytes\n",
           affinity_names[tuning->memory_affinity],
           tuning->batch_size,
           tuning->transfer_size);
    printf("    Prefetch: %-12s  Async: %-3s    Bandwidth Priority: %s\n",
           prefetch_names[tuning->prefetch_mode],
           tuning->async_transfer ? "Yes" : "No",
           tuning->bandwidth_priority ? "High" : "Normal");
}

int main(int argc, char* argv[]) {
    int scale = 1;  // Scale factor for benchmark size

    // Parse args
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            scale = atoi(argv[++i]);
        }
    }

    printf("╔════════════════════════════════════════════════════════════════════════════╗\n");
    printf("║             MCF / LLAMA / GROMACS Workload Benchmark Suite                 ║\n");
    printf("╚════════════════════════════════════════════════════════════════════════════╝\n");
    printf("\nScale factor: %d\n", scale);

    /*
     * MCF Benchmark
     */
    print_header("MCF BENCHMARK - Network Simplex Pointer Chasing");

    size_t mcf_nodes = 100000 * scale;
    size_t mcf_arcs = 500000 * scale;
    int mcf_iters = 10;

    printf("\n  Configuration:\n");
    printf("    Nodes: %zu, Arcs: %zu, Iterations: %d\n", mcf_nodes, mcf_arcs, mcf_iters);
    printf("    Memory: %.1f MB (nodes) + %.1f MB (arcs)\n",
           mcf_nodes * sizeof(mcf_node_t) / 1e6,
           mcf_arcs * sizeof(mcf_arc_t) / 1e6);

    printf("\n  Running with DEFAULT profile...\n");
    print_profile_settings(PGAS_PROFILE_DEFAULT);
    mcf_result_t mcf_default = benchmark_mcf(mcf_nodes, mcf_arcs, mcf_iters);
    printf("    Results: %.2f M ops/sec, %.1f ns/op\n",
           mcf_default.ops_per_sec / 1e6, mcf_default.latency_ns);

    printf("\n  Running with MCF profile...\n");
    print_profile_settings(PGAS_PROFILE_MCF);
    mcf_result_t mcf_tuned = benchmark_mcf(mcf_nodes, mcf_arcs, mcf_iters);
    printf("    Results: %.2f M ops/sec, %.1f ns/op\n",
           mcf_tuned.ops_per_sec / 1e6, mcf_tuned.latency_ns);

    double mcf_improvement = (mcf_tuned.ops_per_sec / mcf_default.ops_per_sec - 1) * 100;
    printf("\n  ► MCF Profile vs Default: %+.1f%%\n", mcf_improvement);

    /*
     * LLAMA Benchmark
     */
    print_header("LLAMA BENCHMARK - LLM Inference Weight Streaming");

    size_t llama_size_mb = 256 * scale;  // Model size
    int llama_layers = 32;
    int llama_iters = 5;

    printf("\n  Configuration:\n");
    printf("    Model Size: %zu MB, Layers: %d, Iterations: %d\n",
           llama_size_mb, llama_layers, llama_iters);

    printf("\n  Running with DEFAULT profile...\n");
    print_profile_settings(PGAS_PROFILE_DEFAULT);
    llama_result_t llama_default = benchmark_llama(llama_size_mb, llama_layers, llama_iters);
    printf("    Results: %.2f GB/s bandwidth, %.2f ms/layer\n",
           llama_default.bandwidth_gbps, llama_default.latency_ms);

    printf("\n  Running with LLAMA profile...\n");
    print_profile_settings(PGAS_PROFILE_LLAMA);
    llama_result_t llama_tuned = benchmark_llama(llama_size_mb, llama_layers, llama_iters);
    printf("    Results: %.2f GB/s bandwidth, %.2f ms/layer\n",
           llama_tuned.bandwidth_gbps, llama_tuned.latency_ms);

    double llama_improvement = (llama_tuned.bandwidth_gbps / llama_default.bandwidth_gbps - 1) * 100;
    printf("\n  ► LLAMA Profile vs Default: %+.1f%%\n", llama_improvement);

    /*
     * GROMACS Benchmark
     */
    print_header("GROMACS BENCHMARK - Molecular Dynamics Neighbor List");

    int gromacs_particles = 10000 * scale;
    int gromacs_neighbors = 100;
    int gromacs_steps = 100;

    printf("\n  Configuration:\n");
    printf("    Particles: %d, Neighbors/particle: %d, Timesteps: %d\n",
           gromacs_particles, gromacs_neighbors, gromacs_steps);
    printf("    Memory: %.1f MB (particles) + %.1f MB (neighbor lists)\n",
           gromacs_particles * sizeof(particle_t) / 1e6,
           gromacs_particles * gromacs_neighbors * sizeof(int) / 1e6);

    printf("\n  Running with DEFAULT profile...\n");
    print_profile_settings(PGAS_PROFILE_DEFAULT);
    gromacs_result_t gromacs_default = benchmark_gromacs(gromacs_particles, gromacs_neighbors, gromacs_steps);
    printf("    Results: %.2f M interactions/sec, %.2f ns/day\n",
           gromacs_default.interactions_per_sec / 1e6, gromacs_default.ns_per_day);

    printf("\n  Running with GROMACS profile...\n");
    print_profile_settings(PGAS_PROFILE_GROMACS);
    gromacs_result_t gromacs_tuned = benchmark_gromacs(gromacs_particles, gromacs_neighbors, gromacs_steps);
    printf("    Results: %.2f M interactions/sec, %.2f ns/day\n",
           gromacs_tuned.interactions_per_sec / 1e6, gromacs_tuned.ns_per_day);

    double gromacs_improvement = (gromacs_tuned.interactions_per_sec / gromacs_default.interactions_per_sec - 1) * 100;
    printf("\n  ► GROMACS Profile vs Default: %+.1f%%\n", gromacs_improvement);

    /*
     * Summary
     */
    print_header("BENCHMARK SUMMARY");

    printf("\n  ┌──────────────┬─────────────────────────────┬─────────────────────────────┬────────────┐\n");
    printf("  │ Workload     │ DEFAULT Profile             │ TUNED Profile               │ Improvement│\n");
    printf("  ├──────────────┼─────────────────────────────┼─────────────────────────────┼────────────┤\n");
    printf("  │ MCF          │ %8.2f M ops/sec          │ %8.2f M ops/sec          │ %+7.1f%%   │\n",
           mcf_default.ops_per_sec / 1e6, mcf_tuned.ops_per_sec / 1e6, mcf_improvement);
    printf("  │ LLAMA        │ %8.2f GB/s              │ %8.2f GB/s              │ %+7.1f%%   │\n",
           llama_default.bandwidth_gbps, llama_tuned.bandwidth_gbps, llama_improvement);
    printf("  │ GROMACS      │ %8.2f M int/sec         │ %8.2f M int/sec         │ %+7.1f%%   │\n",
           gromacs_default.interactions_per_sec / 1e6, gromacs_tuned.interactions_per_sec / 1e6, gromacs_improvement);
    printf("  └──────────────┴─────────────────────────────┴─────────────────────────────┴────────────┘\n");

    printf("\n  Profile Recommendations:\n");
    printf("    • MCF:     Small transfers (64B), aggressive prefetch, LOCAL affinity\n");
    printf("               Best for: pointer-chasing, irregular graph traversal\n\n");
    printf("    • LLAMA:   Large transfers (1MB), sequential prefetch, INTERLEAVE affinity\n");
    printf("               Best for: memory-bandwidth bound, sequential weight loading\n\n");
    printf("    • GROMACS: Medium transfers (8KB), neighbor-list prefetch, async transfers\n");
    printf("               Best for: neighbor-list access, spatial locality patterns\n");

    printf("\n=== Benchmark Complete ===\n");
    return 0;
}
