/*
 * VSAG/HNSW Integration Test
 * Simulates vector similarity search (HNSW algorithm) memory access patterns on PGAS/CXL
 *
 * VSAG/HNSW characteristics:
 * - Vector data: large contiguous arrays (bandwidth-bound distance calculations)
 * - Graph structure: random pointer chasing for neighbor traversal (latency-bound)
 * - Mixed workload: sequential vector reads + random graph traversal
 * - Memory footprint: vectors (N * dim * 4 bytes) + graph (N * M * 8 bytes)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
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
 * HNSW index configuration
 */
typedef struct {
    int num_vectors;        // Number of vectors in the index
    int dim;                // Vector dimension
    int M;                  // Max neighbors per node
    int ef_construction;    // ef during construction
    int ef_search;          // ef during search
    int num_layers;         // Number of layers (log scale)
} hnsw_config_t;

/*
 * HNSW graph node
 */
typedef struct {
    int* neighbors;         // Neighbor IDs [M elements]
    int num_neighbors;      // Actual number of neighbors
    int layer;              // Node's highest layer
} hnsw_node_t;

/*
 * HNSW index structure
 */
typedef struct {
    float* vectors;         // Vector data [num_vectors * dim]
    hnsw_node_t* nodes;     // Graph nodes [num_vectors]
    int* layer_entry_points;// Entry point per layer
    int entry_point;        // Top-level entry point
    hnsw_config_t config;
    size_t vector_mem_size;
    size_t graph_mem_size;
} hnsw_index_t;

/*
 * Benchmark results
 */
typedef struct {
    double build_time_sec;
    double search_time_sec;
    double total_distance_calcs;
    double total_graph_traversals;
    double vector_bandwidth_gbps;
    double graph_latency_us;
    double qps;             // Queries per second
    int recall_at_10;       // Recall@10 (out of 100)
} vsag_result_t;

/*
 * Calculate L2 distance between two vectors
 * This is the main bandwidth-bound operation
 */
static float calc_l2_distance(const float* a, const float* b, int dim) {
    float sum = 0.0f;

    // Prefetch next cache lines
    __builtin_prefetch(a + 64/sizeof(float), 0, 0);
    __builtin_prefetch(b + 64/sizeof(float), 0, 0);

    // Vectorizable loop for distance calculation
    for (int i = 0; i < dim; i += 4) {
        float d0 = a[i] - b[i];
        float d1 = (i+1 < dim) ? a[i+1] - b[i+1] : 0.0f;
        float d2 = (i+2 < dim) ? a[i+2] - b[i+2] : 0.0f;
        float d3 = (i+3 < dim) ? a[i+3] - b[i+3] : 0.0f;
        sum += d0*d0 + d1*d1 + d2*d2 + d3*d3;
    }
    return sum;
}

/*
 * Priority queue for candidate management
 */
typedef struct {
    int id;
    float distance;
} candidate_t;

typedef struct {
    candidate_t* data;
    int size;
    int capacity;
} pqueue_t;

static void pqueue_init(pqueue_t* pq, int capacity) {
    pq->data = malloc(capacity * sizeof(candidate_t));
    pq->size = 0;
    pq->capacity = capacity;
}

static void pqueue_push(pqueue_t* pq, int id, float dist) {
    if (pq->size >= pq->capacity) {
        // Replace worst if new is better
        if (dist < pq->data[0].distance) {
            pq->data[0].id = id;
            pq->data[0].distance = dist;
            // Bubble down (simplified max-heap)
            int i = 0;
            while (2*i + 1 < pq->size) {
                int child = 2*i + 1;
                if (child + 1 < pq->size && pq->data[child+1].distance > pq->data[child].distance)
                    child++;
                if (pq->data[i].distance >= pq->data[child].distance) break;
                candidate_t tmp = pq->data[i];
                pq->data[i] = pq->data[child];
                pq->data[child] = tmp;
                i = child;
            }
        }
        return;
    }

    // Insert new element
    int i = pq->size++;
    pq->data[i].id = id;
    pq->data[i].distance = dist;

    // Bubble up
    while (i > 0) {
        int parent = (i - 1) / 2;
        if (pq->data[parent].distance >= pq->data[i].distance) break;
        candidate_t tmp = pq->data[i];
        pq->data[i] = pq->data[parent];
        pq->data[parent] = tmp;
        i = parent;
    }
}

static void pqueue_free(pqueue_t* pq) {
    free(pq->data);
}

/*
 * Initialize HNSW index with random vectors and graph structure
 */
static hnsw_index_t* create_hnsw_index(hnsw_config_t* config) {
    hnsw_index_t* index = malloc(sizeof(hnsw_index_t));
    index->config = *config;

    int n = config->num_vectors;
    int dim = config->dim;
    int M = config->M;

    // Calculate memory sizes
    index->vector_mem_size = (size_t)n * dim * sizeof(float);
    index->graph_mem_size = (size_t)n * (sizeof(hnsw_node_t) + M * sizeof(int));

    printf("  HNSW Index Configuration:\n");
    printf("    Vectors: %d, Dimension: %d\n", n, dim);
    printf("    M (max neighbors): %d, ef_search: %d\n", M, config->ef_search);
    printf("    Vector memory: %.1f MB\n", index->vector_mem_size / (double)MB);
    printf("    Graph memory: %.1f MB\n", index->graph_mem_size / (double)MB);
    printf("    Total memory: %.1f MB\n",
           (index->vector_mem_size + index->graph_mem_size) / (double)MB);

    // Allocate vector storage using mmap (CXL-like behavior)
    printf("  Allocating vectors...\n");
    index->vectors = mmap(NULL, index->vector_mem_size,
                          PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (index->vectors == MAP_FAILED) {
        perror("mmap vectors failed");
        free(index);
        return NULL;
    }

    // Initialize with random vectors
    srand(42);
    for (int i = 0; i < n * dim; i++) {
        index->vectors[i] = (float)rand() / RAND_MAX;
    }

    // Allocate graph structure
    printf("  Allocating graph structure...\n");
    index->nodes = mmap(NULL, n * sizeof(hnsw_node_t),
                        PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (index->nodes == MAP_FAILED) {
        perror("mmap nodes failed");
        munmap(index->vectors, index->vector_mem_size);
        free(index);
        return NULL;
    }

    // Allocate neighbor lists
    for (int i = 0; i < n; i++) {
        index->nodes[i].neighbors = malloc(M * sizeof(int));
        index->nodes[i].num_neighbors = 0;
        index->nodes[i].layer = 0;
    }

    // Build HNSW graph (simplified - random connections)
    printf("  Building HNSW graph...\n");
    double build_start = get_time_sec();

    index->entry_point = 0;

    for (int i = 1; i < n; i++) {
        // Find neighbors by distance (simplified - use random sample)
        int sample_size = M * 4;
        int* samples = malloc(sample_size * sizeof(int));

        // Sample random candidates
        for (int j = 0; j < sample_size; j++) {
            samples[j] = rand() % i;
        }

        // Calculate distances and keep best M
        float* sample_dists = malloc(sample_size * sizeof(float));
        float* vec_i = &index->vectors[i * dim];

        for (int j = 0; j < sample_size; j++) {
            float* vec_j = &index->vectors[samples[j] * dim];
            sample_dists[j] = calc_l2_distance(vec_i, vec_j, dim);
        }

        // Simple selection of M best
        for (int k = 0; k < M && k < sample_size; k++) {
            int best = k;
            for (int j = k + 1; j < sample_size; j++) {
                if (sample_dists[j] < sample_dists[best]) best = j;
            }
            if (best != k) {
                int tmp_s = samples[k]; samples[k] = samples[best]; samples[best] = tmp_s;
                float tmp_d = sample_dists[k]; sample_dists[k] = sample_dists[best]; sample_dists[best] = tmp_d;
            }

            // Add bidirectional connection
            int neighbor = samples[k];
            if (index->nodes[i].num_neighbors < M) {
                index->nodes[i].neighbors[index->nodes[i].num_neighbors++] = neighbor;
            }
            if (index->nodes[neighbor].num_neighbors < M) {
                index->nodes[neighbor].neighbors[index->nodes[neighbor].num_neighbors++] = i;
            }
        }

        free(samples);
        free(sample_dists);

        if (i % 100000 == 0) {
            printf("    Built %d nodes (%.1f%%)\n", i, 100.0 * i / n);
        }
    }

    double build_time = get_time_sec() - build_start;
    printf("  Graph build time: %.2f seconds\n", build_time);

    return index;
}

/*
 * HNSW search - greedy beam search
 * This is where we see mixed memory access patterns:
 * - Graph traversal: random pointer chasing (latency-bound)
 * - Distance calculation: sequential vector reads (bandwidth-bound)
 */
static void hnsw_search(hnsw_index_t* index, float* query, int k,
                       int* results, float* distances,
                       int* num_distance_calcs, int* num_graph_accesses) {
    int ef = index->config.ef_search;
    int dim = index->config.dim;
    int n = index->config.num_vectors;

    *num_distance_calcs = 0;
    *num_graph_accesses = 0;

    // Visited set (bitmap)
    int visited_size = (n + 63) / 64;
    uint64_t* visited = calloc(visited_size, sizeof(uint64_t));

    // Priority queue for candidates
    pqueue_t candidates;
    pqueue_init(&candidates, ef);

    // Start from entry point
    int ep = index->entry_point;
    float* vec_ep = &index->vectors[ep * dim];
    float dist_ep = calc_l2_distance(query, vec_ep, dim);
    (*num_distance_calcs)++;

    pqueue_push(&candidates, ep, dist_ep);
    visited[ep / 64] |= (1ULL << (ep % 64));

    // Greedy search
    int changed = 1;
    while (changed) {
        changed = 0;

        // Process all candidates
        for (int ci = 0; ci < candidates.size; ci++) {
            int current = candidates.data[ci].id;
            hnsw_node_t* node = &index->nodes[current];
            (*num_graph_accesses)++;

            // Check all neighbors (random access pattern)
            for (int ni = 0; ni < node->num_neighbors; ni++) {
                int neighbor = node->neighbors[ni];
                (*num_graph_accesses)++;

                // Check if visited
                if (visited[neighbor / 64] & (1ULL << (neighbor % 64))) continue;
                visited[neighbor / 64] |= (1ULL << (neighbor % 64));

                // Calculate distance (sequential vector access)
                float* vec_n = &index->vectors[neighbor * dim];
                float dist = calc_l2_distance(query, vec_n, dim);
                (*num_distance_calcs)++;

                // Add to candidates
                if (candidates.size < ef || dist < candidates.data[0].distance) {
                    pqueue_push(&candidates, neighbor, dist);
                    changed = 1;
                }
            }
        }
    }

    // Extract top-k results
    for (int i = 0; i < k && i < candidates.size; i++) {
        results[i] = candidates.data[i].id;
        distances[i] = candidates.data[i].distance;
    }

    pqueue_free(&candidates);
    free(visited);
}

/*
 * Destroy HNSW index
 */
static void destroy_hnsw_index(hnsw_index_t* index) {
    for (int i = 0; i < index->config.num_vectors; i++) {
        free(index->nodes[i].neighbors);
    }
    munmap(index->nodes, index->config.num_vectors * sizeof(hnsw_node_t));
    munmap(index->vectors, index->vector_mem_size);
    free(index);
}

/*
 * Run VSAG/HNSW benchmark simulation
 */
vsag_result_t run_vsag_simulation(hnsw_config_t* config, int num_queries) {
    vsag_result_t result = {0};

    printf("\n========================================\n");
    printf("VSAG/HNSW Simulation\n");
    printf("========================================\n\n");

    // Create index
    double start = get_time_sec();
    hnsw_index_t* index = create_hnsw_index(config);
    if (!index) {
        fprintf(stderr, "Failed to create index\n");
        return result;
    }
    result.build_time_sec = get_time_sec() - start;

    // Generate random queries
    printf("\n  Running %d search queries (ef_search=%d)...\n",
           num_queries, config->ef_search);

    float* queries = malloc(num_queries * config->dim * sizeof(float));
    for (int i = 0; i < num_queries * config->dim; i++) {
        queries[i] = (float)rand() / RAND_MAX;
    }

    // Run searches
    int k = 10;
    int* results = malloc(k * sizeof(int));
    float* distances = malloc(k * sizeof(float));

    int total_distance_calcs = 0;
    int total_graph_accesses = 0;

    start = get_time_sec();

    for (int q = 0; q < num_queries; q++) {
        float* query = &queries[q * config->dim];
        int dist_calcs, graph_accesses;

        hnsw_search(index, query, k, results, distances, &dist_calcs, &graph_accesses);

        total_distance_calcs += dist_calcs;
        total_graph_accesses += graph_accesses;
    }

    result.search_time_sec = get_time_sec() - start;
    result.total_distance_calcs = total_distance_calcs;
    result.total_graph_traversals = total_graph_accesses;
    result.qps = num_queries / result.search_time_sec;

    // Calculate bandwidth (vector reads for distance calculation)
    size_t bytes_read = (size_t)total_distance_calcs * config->dim * sizeof(float);
    result.vector_bandwidth_gbps = (bytes_read / result.search_time_sec) / GB;

    // Calculate average graph latency
    result.graph_latency_us = (result.search_time_sec * 1e6) / total_graph_accesses;

    printf("\n========================================\n");
    printf("Benchmark Results\n");
    printf("========================================\n");
    printf("  Build time:           %.2f seconds\n", result.build_time_sec);
    printf("  Search time:          %.2f seconds\n", result.search_time_sec);
    printf("  Queries per second:   %.1f QPS\n", result.qps);
    printf("  Distance calculations: %.0f (%.1f per query)\n",
           result.total_distance_calcs, result.total_distance_calcs / num_queries);
    printf("  Graph accesses:       %.0f (%.1f per query)\n",
           result.total_graph_traversals, result.total_graph_traversals / num_queries);
    printf("  Vector bandwidth:     %.2f GB/s\n", result.vector_bandwidth_gbps);
    printf("  Avg graph latency:    %.2f us\n", result.graph_latency_us);
    printf("========================================\n\n");

    // Cleanup
    free(queries);
    free(results);
    free(distances);
    destroy_hnsw_index(index);

    return result;
}

/*
 * Compare with different PGAS profiles
 */
void compare_pgas_profiles(hnsw_config_t* config) {
    printf("\n########################################\n");
    printf("# PGAS Profile Comparison for VSAG    #\n");
    printf("########################################\n");

    // Define custom VSAG profile (hybrid of MCF + LLAMA characteristics)
    pgas_tuning_t vsag_tuning = {
        .memory_affinity = PGAS_AFFINITY_INTERLEAVE,  // Interleave vectors across nodes
        .partition_scheme = PGAS_PARTITION_BLOCK,     // Block partition for vectors
        .cache_line_align = true,                     // Align for efficient access
        .numa_bind = true,
        .batch_size = 128,                            // Batch distance calculations
        .transfer_size = 4096,                        // 4KB transfers for vectors
        .prefetch_mode = PGAS_PREFETCH_SEQUENTIAL,    // Sequential for vectors
        .consistency = PGAS_CONSISTENCY_RELAXED,
        .num_threads = 8,
        .bandwidth_priority = true,                   // Bandwidth matters for vectors
        .async_transfer = true
    };

    printf("\nVSAG PGAS Profile:\n");
    printf("  - Memory: Interleaved (distribute vectors across CXL nodes)\n");
    printf("  - Prefetch: Sequential (for distance calculations)\n");
    printf("  - Batch size: 128 (batch neighbor lookups)\n");
    printf("  - Transfer size: 4KB (optimal for vector blocks)\n");
    printf("  - Priority: Bandwidth (distance calc is bandwidth-bound)\n");

    // Note: In a real CXL system, we would:
    // 1. Store vectors in CXL memory (large, bandwidth-bound)
    // 2. Store graph structure in local DRAM (small, latency-sensitive)
    // 3. Use prefetching for vector access patterns
    // 4. Batch remote vector fetches during search

    printf("\nMemory Layout Recommendation:\n");
    printf("  - Vectors (%.1f MB): CXL memory (bandwidth-optimized)\n",
           (double)config->num_vectors * config->dim * sizeof(float) / MB);
    printf("  - Graph (%.1f MB): Local DRAM (latency-optimized)\n",
           (double)config->num_vectors * config->M * sizeof(int) / MB);
    printf("  - Candidate queues: Local DRAM (frequent updates)\n");
}

int main(int argc, char** argv) {
    printf("========================================\n");
    printf("VSAG/HNSW PGAS Integration Test\n");
    printf("========================================\n\n");

    // Default configuration (SIFT-like)
    hnsw_config_t config = {
        .num_vectors = 100000,   // 100K vectors
        .dim = 128,              // 128 dimensions (SIFT)
        .M = 16,                 // 16 neighbors per node
        .ef_construction = 200,
        .ef_search = 100,
        .num_layers = 6
    };

    // Parse command line arguments
    if (argc > 1) config.num_vectors = atoi(argv[1]);
    if (argc > 2) config.dim = atoi(argv[2]);
    if (argc > 3) config.M = atoi(argv[3]);
    if (argc > 4) config.ef_search = atoi(argv[4]);

    int num_queries = 1000;
    if (argc > 5) num_queries = atoi(argv[5]);

    // Run benchmark
    vsag_result_t result = run_vsag_simulation(&config, num_queries);

    // Show PGAS profile recommendations
    compare_pgas_profiles(&config);

    printf("\n========================================\n");
    printf("Test completed successfully!\n");
    printf("========================================\n");

    return 0;
}
