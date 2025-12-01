/*
 * VSAG/HNSW PGAS Benchmark with CXL Memory Simulation
 *
 * This benchmark simulates VSAG/HNSW workload on PGAS with different
 * memory configurations to evaluate CXL performance.
 *
 * Key characteristics:
 * - Vector data stored in CXL memory (bandwidth-optimized)
 * - Graph structure in local memory or CXL (latency-sensitive)
 * - Mixed access patterns: sequential (vectors) + random (graph)
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
 * HNSW configuration
 */
typedef struct {
    int num_vectors;
    int dim;
    int M;                  // Max neighbors per level
    int ef_search;          // Search beam width
} hnsw_config_t;

/*
 * Benchmark results
 */
typedef struct {
    double build_time;
    double search_time;
    double vector_read_time;
    double graph_access_time;
    size_t vector_bytes_read;
    size_t graph_bytes_read;
    double qps;
    double vector_bandwidth_gbps;
    double recall;
} benchmark_result_t;

/*
 * PGAS-aware HNSW index
 */
typedef struct {
    // PGAS context
    pgas_context_t* ctx;

    // Vector data in PGAS (CXL memory)
    pgas_ptr_t vectors_ptr;     // Global pointer to vector array
    float* vectors_local;        // Local mapping (for local node)

    // Graph structure (can be local or PGAS)
    int** neighbors;             // Neighbor lists
    int* neighbor_counts;        // Number of neighbors per node
    int entry_point;

    // Configuration
    hnsw_config_t config;

    // Memory sizes
    size_t vector_mem_size;
    size_t graph_mem_size;
} pgas_hnsw_t;

/*
 * Calculate L2 distance between vectors
 */
static inline float calc_l2_distance(const float* a, const float* b, int dim) {
    float sum = 0.0f;

    // Prefetch for sequential access
    __builtin_prefetch(a + 16, 0, 0);
    __builtin_prefetch(b + 16, 0, 0);

    for (int i = 0; i < dim; i += 8) {
        float d0 = a[i] - b[i];
        float d1 = (i+1 < dim) ? a[i+1] - b[i+1] : 0.0f;
        float d2 = (i+2 < dim) ? a[i+2] - b[i+2] : 0.0f;
        float d3 = (i+3 < dim) ? a[i+3] - b[i+3] : 0.0f;
        float d4 = (i+4 < dim) ? a[i+4] - b[i+4] : 0.0f;
        float d5 = (i+5 < dim) ? a[i+5] - b[i+5] : 0.0f;
        float d6 = (i+6 < dim) ? a[i+6] - b[i+6] : 0.0f;
        float d7 = (i+7 < dim) ? a[i+7] - b[i+7] : 0.0f;
        sum += d0*d0 + d1*d1 + d2*d2 + d3*d3 + d4*d4 + d5*d5 + d6*d6 + d7*d7;
    }
    return sum;
}

/*
 * Get vector from PGAS (simulates CXL read)
 */
static inline float* pgas_get_vector(pgas_hnsw_t* index, int id) {
    // In a real distributed system, this would check if vector is local
    // and potentially fetch from remote CXL memory
    return index->vectors_local + (size_t)id * index->config.dim;
}

/*
 * Candidate priority queue
 */
typedef struct {
    int id;
    float dist;
} candidate_t;

typedef struct {
    candidate_t* data;
    int size;
    int capacity;
} pqueue_t;

static void pq_init(pqueue_t* pq, int capacity) {
    pq->data = malloc(capacity * sizeof(candidate_t));
    pq->size = 0;
    pq->capacity = capacity;
}

static void pq_push(pqueue_t* pq, int id, float dist) {
    if (pq->size >= pq->capacity) {
        if (dist >= pq->data[0].dist) return;
        pq->data[0].id = id;
        pq->data[0].dist = dist;
        int i = 0;
        while (2*i+1 < pq->size) {
            int c = 2*i+1;
            if (c+1 < pq->size && pq->data[c+1].dist > pq->data[c].dist) c++;
            if (pq->data[i].dist >= pq->data[c].dist) break;
            candidate_t t = pq->data[i];
            pq->data[i] = pq->data[c];
            pq->data[c] = t;
            i = c;
        }
        return;
    }
    int i = pq->size++;
    pq->data[i].id = id;
    pq->data[i].dist = dist;
    while (i > 0) {
        int p = (i-1)/2;
        if (pq->data[p].dist >= pq->data[i].dist) break;
        candidate_t t = pq->data[i];
        pq->data[i] = pq->data[p];
        pq->data[p] = t;
        i = p;
    }
}

static void pq_free(pqueue_t* pq) { free(pq->data); }

/*
 * Initialize PGAS-aware HNSW index
 */
pgas_hnsw_t* pgas_hnsw_create(pgas_context_t* ctx, hnsw_config_t* config) {
    pgas_hnsw_t* index = calloc(1, sizeof(pgas_hnsw_t));
    index->ctx = ctx;
    index->config = *config;

    int n = config->num_vectors;
    int dim = config->dim;
    int M = config->M;

    // Calculate memory sizes
    index->vector_mem_size = (size_t)n * dim * sizeof(float);
    index->graph_mem_size = (size_t)n * M * sizeof(int) + n * sizeof(int);

    printf("  PGAS HNSW Index Configuration:\n");
    printf("    Vectors: %d, Dimension: %d\n", n, dim);
    printf("    M: %d, ef_search: %d\n", M, config->ef_search);
    printf("    Vector memory (CXL): %.1f MB\n", index->vector_mem_size / (double)MB);
    printf("    Graph memory (local): %.1f MB\n", index->graph_mem_size / (double)MB);

    // Allocate vectors in PGAS (simulates CXL memory)
    printf("  Allocating vectors in PGAS/CXL...\n");
    index->vectors_ptr = pgas_alloc(ctx, index->vector_mem_size, PGAS_AFFINITY_INTERLEAVE);

    if (pgas_ptr_is_null(index->vectors_ptr)) {
        // Fall back to mmap if PGAS allocation fails
        printf("  PGAS alloc failed, using mmap fallback...\n");
        index->vectors_local = mmap(NULL, index->vector_mem_size,
                                    PROT_READ | PROT_WRITE,
                                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    } else {
        index->vectors_local = pgas_local_ptr(ctx, index->vectors_ptr);
    }

    if (!index->vectors_local || index->vectors_local == MAP_FAILED) {
        fprintf(stderr, "Failed to allocate vectors\n");
        free(index);
        return NULL;
    }

    // Initialize random vectors
    srand(42);
    for (size_t i = 0; i < (size_t)n * dim; i++) {
        index->vectors_local[i] = (float)rand() / RAND_MAX;
    }

    // Allocate graph (local memory for low latency)
    printf("  Allocating graph structure (local memory)...\n");
    index->neighbors = malloc(n * sizeof(int*));
    index->neighbor_counts = calloc(n, sizeof(int));

    for (int i = 0; i < n; i++) {
        index->neighbors[i] = malloc(M * sizeof(int));
    }

    // Build HNSW graph (simplified)
    printf("  Building HNSW graph...\n");
    double build_start = get_time_sec();

    index->entry_point = 0;

    for (int i = 1; i < n; i++) {
        float* vec_i = pgas_get_vector(index, i);

        // Sample candidates and find nearest
        int sample_size = M * 2;
        int best_neighbors[16];  // Top neighbors
        float best_dists[16];
        int num_best = 0;

        for (int j = 0; j < sample_size && j < i; j++) {
            int cand = rand() % i;
            float* vec_c = pgas_get_vector(index, cand);
            float dist = calc_l2_distance(vec_i, vec_c, dim);

            // Insert into top-M
            int insert_pos = num_best;
            for (int k = 0; k < num_best && k < M; k++) {
                if (dist < best_dists[k]) {
                    insert_pos = k;
                    break;
                }
            }

            if (insert_pos < M) {
                // Shift and insert
                for (int k = (num_best < M-1 ? num_best : M-1); k > insert_pos; k--) {
                    best_neighbors[k] = best_neighbors[k-1];
                    best_dists[k] = best_dists[k-1];
                }
                best_neighbors[insert_pos] = cand;
                best_dists[insert_pos] = dist;
                if (num_best < M) num_best++;
            }
        }

        // Add connections
        for (int j = 0; j < num_best; j++) {
            int neighbor = best_neighbors[j];

            if (index->neighbor_counts[i] < M) {
                index->neighbors[i][index->neighbor_counts[i]++] = neighbor;
            }
            if (index->neighbor_counts[neighbor] < M) {
                index->neighbors[neighbor][index->neighbor_counts[neighbor]++] = i;
            }
        }

        if (i % 50000 == 0) {
            printf("    Built %d/%d nodes (%.1f%%)\n", i, n, 100.0 * i / n);
        }
    }

    double build_time = get_time_sec() - build_start;
    printf("  Graph build time: %.2f seconds\n", build_time);

    return index;
}

/*
 * HNSW search with PGAS
 */
int pgas_hnsw_search(pgas_hnsw_t* index, float* query, int k,
                     int* results, float* distances,
                     size_t* vector_bytes, size_t* graph_bytes) {
    int ef = index->config.ef_search;
    int dim = index->config.dim;
    int n = index->config.num_vectors;

    *vector_bytes = 0;
    *graph_bytes = 0;

    // Visited bitmap
    int vis_size = (n + 63) / 64;
    uint64_t* visited = calloc(vis_size, sizeof(uint64_t));

    // Priority queue for candidates
    pqueue_t cands;
    pq_init(&cands, ef);

    // Start from entry point
    float* ep_vec = pgas_get_vector(index, index->entry_point);
    *vector_bytes += dim * sizeof(float);

    float ep_dist = calc_l2_distance(query, ep_vec, dim);
    pq_push(&cands, index->entry_point, ep_dist);
    visited[index->entry_point / 64] |= (1ULL << (index->entry_point % 64));

    // Greedy search
    int changed = 1;
    int iterations = 0;
    while (changed && iterations < 50) {
        changed = 0;
        iterations++;

        for (int ci = 0; ci < cands.size; ci++) {
            int curr = cands.data[ci].id;
            *graph_bytes += sizeof(int);

            // Explore neighbors
            for (int ni = 0; ni < index->neighbor_counts[curr]; ni++) {
                int neighbor = index->neighbors[curr][ni];
                *graph_bytes += sizeof(int);

                if (visited[neighbor / 64] & (1ULL << (neighbor % 64))) continue;
                visited[neighbor / 64] |= (1ULL << (neighbor % 64));

                // Get vector from PGAS (CXL read)
                float* n_vec = pgas_get_vector(index, neighbor);
                *vector_bytes += dim * sizeof(float);

                float dist = calc_l2_distance(query, n_vec, dim);

                if (cands.size < ef || dist < cands.data[0].dist) {
                    pq_push(&cands, neighbor, dist);
                    changed = 1;
                }
            }
        }
    }

    // Extract results
    for (int i = 0; i < k && i < cands.size; i++) {
        results[i] = cands.data[i].id;
        distances[i] = cands.data[i].dist;
    }

    pq_free(&cands);
    free(visited);

    return cands.size < k ? cands.size : k;
}

/*
 * Free PGAS HNSW index
 */
void pgas_hnsw_destroy(pgas_hnsw_t* index) {
    for (int i = 0; i < index->config.num_vectors; i++) {
        free(index->neighbors[i]);
    }
    free(index->neighbors);
    free(index->neighbor_counts);

    if (!pgas_ptr_is_null(index->vectors_ptr)) {
        pgas_free(index->ctx, index->vectors_ptr);
    } else {
        munmap(index->vectors_local, index->vector_mem_size);
    }

    free(index);
}

/*
 * Run benchmark with different PGAS profiles
 */
benchmark_result_t run_benchmark(pgas_context_t* ctx, hnsw_config_t* config,
                                 int num_queries, const char* profile_name) {
    benchmark_result_t result = {0};

    printf("\n========================================\n");
    printf("VSAG PGAS Benchmark - %s\n", profile_name);
    printf("========================================\n\n");

    // Create index
    double start = get_time_sec();
    pgas_hnsw_t* index = pgas_hnsw_create(ctx, config);
    if (!index) {
        fprintf(stderr, "Failed to create index\n");
        return result;
    }
    result.build_time = get_time_sec() - start;

    // Generate queries
    printf("\n  Running %d queries...\n", num_queries);
    float* queries = malloc(num_queries * config->dim * sizeof(float));
    for (int i = 0; i < num_queries * config->dim; i++) {
        queries[i] = (float)rand() / RAND_MAX;
    }

    int k = 10;
    int* results = malloc(k * sizeof(int));
    float* distances = malloc(k * sizeof(float));

    size_t total_vec_bytes = 0;
    size_t total_graph_bytes = 0;

    start = get_time_sec();
    for (int q = 0; q < num_queries; q++) {
        float* query = queries + q * config->dim;
        size_t vec_bytes, graph_bytes;

        pgas_hnsw_search(index, query, k, results, distances, &vec_bytes, &graph_bytes);

        total_vec_bytes += vec_bytes;
        total_graph_bytes += graph_bytes;
    }
    result.search_time = get_time_sec() - start;

    result.vector_bytes_read = total_vec_bytes;
    result.graph_bytes_read = total_graph_bytes;
    result.qps = num_queries / result.search_time;
    result.vector_bandwidth_gbps = (total_vec_bytes / result.search_time) / GB;

    // Get PGAS statistics
    pgas_stats_t pgas_stats;
    pgas_get_stats(ctx, &pgas_stats);

    printf("\n========================================\n");
    printf("Results - %s\n", profile_name);
    printf("========================================\n");
    printf("  Build time:           %.2f sec\n", result.build_time);
    printf("  Search time:          %.4f sec\n", result.search_time);
    printf("  QPS:                  %.1f\n", result.qps);
    printf("  Vector bytes read:    %.1f MB\n", total_vec_bytes / (double)MB);
    printf("  Graph bytes read:     %.1f MB\n", total_graph_bytes / (double)MB);
    printf("  Vector bandwidth:     %.2f GB/s\n", result.vector_bandwidth_gbps);
    printf("  PGAS remote reads:    %lu\n", pgas_stats.remote_reads);
    printf("  PGAS remote writes:   %lu\n", pgas_stats.remote_writes);
    printf("  Bytes transferred:    %lu\n", pgas_stats.bytes_transferred);
    printf("========================================\n");

    free(queries);
    free(results);
    free(distances);
    pgas_hnsw_destroy(index);

    return result;
}

int main(int argc, char** argv) {
    printf("########################################\n");
    printf("# VSAG PGAS/CXL Benchmark              #\n");
    printf("########################################\n\n");

    // Initialize PGAS
    pgas_context_t ctx;
    const char* config_file = argc > 1 ? argv[1] : "../config/node0.conf";

    printf("Initializing PGAS with config: %s\n", config_file);
    if (pgas_init(&ctx, config_file) != 0) {
        fprintf(stderr, "PGAS initialization failed\n");
        return 1;
    }

    // HNSW configuration
    hnsw_config_t config = {
        .num_vectors = 100000,
        .dim = 128,
        .M = 16,
        .ef_search = 100
    };

    int num_queries = 1000;

    if (argc > 2) config.num_vectors = atoi(argv[2]);
    if (argc > 3) config.dim = atoi(argv[3]);
    if (argc > 4) config.M = atoi(argv[4]);
    if (argc > 5) config.ef_search = atoi(argv[5]);
    if (argc > 6) num_queries = atoi(argv[6]);

    // Run with default profile
    printf("\n--- Testing with DEFAULT profile ---\n");
    pgas_load_profile(&ctx, PGAS_PROFILE_DEFAULT);
    benchmark_result_t default_result = run_benchmark(&ctx, &config, num_queries, "DEFAULT");

    // Run with MCF profile (pointer-chasing optimized)
    printf("\n--- Testing with MCF profile (latency-optimized) ---\n");
    pgas_load_profile(&ctx, PGAS_PROFILE_MCF);
    benchmark_result_t mcf_result = run_benchmark(&ctx, &config, num_queries, "MCF");

    // Run with LLAMA profile (bandwidth-optimized)
    printf("\n--- Testing with LLAMA profile (bandwidth-optimized) ---\n");
    pgas_load_profile(&ctx, PGAS_PROFILE_LLAMA);
    benchmark_result_t llama_result = run_benchmark(&ctx, &config, num_queries, "LLAMA");

    // Summary
    printf("\n########################################\n");
    printf("# Profile Comparison Summary          #\n");
    printf("########################################\n");
    printf("Profile     | QPS        | BW (GB/s) | Build (s)\n");
    printf("------------|------------|-----------|----------\n");
    printf("DEFAULT     | %10.1f | %9.2f | %8.2f\n",
           default_result.qps, default_result.vector_bandwidth_gbps, default_result.build_time);
    printf("MCF         | %10.1f | %9.2f | %8.2f\n",
           mcf_result.qps, mcf_result.vector_bandwidth_gbps, mcf_result.build_time);
    printf("LLAMA       | %10.1f | %9.2f | %8.2f\n",
           llama_result.qps, llama_result.vector_bandwidth_gbps, llama_result.build_time);

    printf("\nRecommendation for VSAG:\n");
    printf("  - Use LLAMA profile for vector-heavy workloads (bandwidth priority)\n");
    printf("  - Use MCF profile for graph-heavy workloads (latency priority)\n");
    printf("  - Hybrid approach: vectors in CXL, graph in local DRAM\n");

    pgas_finalize(&ctx);

    printf("\nBenchmark completed successfully!\n");
    return 0;
}
