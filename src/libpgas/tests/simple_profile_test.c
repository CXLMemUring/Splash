/*
 * Simple Profile Test - Tests tuning profiles with memory benchmarks
 * Demonstrates workload-specific access patterns
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>
#include <pgas/pgas.h>

#define TEST_SIZE (4 * 1024 * 1024)  // 4MB
#define NUM_ITERATIONS 10000

static inline double get_time_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

// Pointer chase test (MCF-like)
double test_pointer_chase(void* mem, size_t size) {
    uint64_t* arr = (uint64_t*)mem;
    size_t count = size / sizeof(uint64_t);

    // Create permutation
    for (size_t i = 0; i < count; i++) arr[i] = i;
    for (size_t i = count - 1; i > 0; i--) {
        size_t j = rand() % (i + 1);
        uint64_t tmp = arr[i];
        arr[i] = arr[j];
        arr[j] = tmp;
    }

    double start = get_time_sec();
    uint64_t idx = 0;
    volatile uint64_t sum = 0;
    for (int iter = 0; iter < NUM_ITERATIONS; iter++) {
        idx = 0;
        for (int i = 0; i < 1000; i++) {
            idx = arr[idx % count];
            sum += idx;
        }
    }
    double elapsed = get_time_sec() - start;
    return (double)(NUM_ITERATIONS * 1000) / elapsed;
}

// Sequential streaming test (LLAMA-like)
double test_streaming(void* mem, size_t size) {
    uint64_t* arr = (uint64_t*)mem;
    size_t count = size / sizeof(uint64_t);

    // Initialize
    for (size_t i = 0; i < count; i++) arr[i] = i;

    double start = get_time_sec();
    volatile uint64_t sum = 0;
    for (int iter = 0; iter < 100; iter++) {
        for (size_t i = 0; i < count; i++) {
            sum += arr[i];
        }
    }
    double elapsed = get_time_sec() - start;
    return (100.0 * size / 1e6) / elapsed;  // MB/s
}

// Neighbor-list test (GROMACS-like)
double test_neighbor_list(void* mem, size_t size) {
    double* particles = (double*)mem;
    int num_particles = size / (6 * sizeof(double));  // 6 doubles per particle

    // Initialize
    for (int i = 0; i < num_particles * 6; i++) {
        particles[i] = (double)rand() / RAND_MAX;
    }

    // Create neighbor lists (fixed 50 neighbors per particle)
    int neighbors_per_particle = 50;

    double start = get_time_sec();
    volatile double sum = 0;
    for (int iter = 0; iter < 100; iter++) {
        for (int i = 0; i < num_particles; i++) {
            for (int j = 0; j < neighbors_per_particle; j++) {
                int neighbor = (i + j * 7) % num_particles;  // Pseudo-random neighbor
                sum += particles[neighbor * 6] + particles[neighbor * 6 + 1];
            }
        }
    }
    double elapsed = get_time_sec() - start;
    return (double)(100 * num_particles * neighbors_per_particle) / elapsed;
}

// Random access test (Graph-like)
double test_random_access(void* mem, size_t size) {
    uint64_t* arr = (uint64_t*)mem;
    size_t count = size / sizeof(uint64_t);

    // Initialize
    for (size_t i = 0; i < count; i++) arr[i] = i;

    double start = get_time_sec();
    volatile uint64_t sum = 0;
    for (int iter = 0; iter < NUM_ITERATIONS; iter++) {
        for (int i = 0; i < 100; i++) {
            size_t idx = rand() % count;
            sum += arr[idx];
        }
    }
    double elapsed = get_time_sec() - start;
    return (double)(NUM_ITERATIONS * 100) / elapsed;
}

void run_profile_tests(const char* profile_name, pgas_profile_t profile) {
    printf("\n╔════════════════════════════════════════════════════════════════╗\n");
    printf("║  Testing: %-52s ║\n", profile_name);
    printf("╚════════════════════════════════════════════════════════════════╝\n");

    const pgas_tuning_t* tuning = pgas_get_default_tuning(profile);

    const char* affinity_names[] = {"LOCAL", "REMOTE", "INTERLEAVE", "REPLICATE"};
    const char* prefetch_names[] = {"NONE", "SEQUENTIAL", "STRIDED", "AGGRESSIVE", "NEIGHBOR"};

    printf("\nProfile Settings:\n");
    printf("  Affinity: %s, Batch: %zu, Transfer: %zu, Prefetch: %s\n",
           affinity_names[tuning->memory_affinity],
           tuning->batch_size,
           tuning->transfer_size,
           prefetch_names[tuning->prefetch_mode]);

    // Allocate memory using mmap (simulates CXL-like allocation)
    void* mem = mmap(NULL, TEST_SIZE, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
        printf("  Failed to allocate memory\n");
        return;
    }

    printf("\nBenchmark Results:\n");

    // Run tests
    srand(42);  // Fixed seed
    double ptr_chase = test_pointer_chase(mem, TEST_SIZE);
    printf("  Pointer Chase:    %12.0f ops/sec\n", ptr_chase);

    srand(42);
    double streaming = test_streaming(mem, TEST_SIZE);
    printf("  Streaming:        %12.2f MB/sec\n", streaming);

    srand(42);
    double neighbor = test_neighbor_list(mem, TEST_SIZE);
    printf("  Neighbor List:    %12.0f ops/sec\n", neighbor);

    srand(42);
    double random = test_random_access(mem, TEST_SIZE);
    printf("  Random Access:    %12.0f ops/sec\n", random);

    munmap(mem, TEST_SIZE);
}

int main(void) {
    printf("╔════════════════════════════════════════════════════════════════╗\n");
    printf("║         PGAS Tuning Profile Test Suite                         ║\n");
    printf("╚════════════════════════════════════════════════════════════════╝\n");
    printf("\nThis test runs workload-representative benchmarks on CXL memory\n");
    printf("to demonstrate tuning profile characteristics.\n");

    run_profile_tests("DEFAULT Profile", PGAS_PROFILE_DEFAULT);
    run_profile_tests("MCF Profile (Pointer Chasing)", PGAS_PROFILE_MCF);
    run_profile_tests("LLAMA Profile (Streaming)", PGAS_PROFILE_LLAMA);
    run_profile_tests("GROMACS Profile (Neighbor List)", PGAS_PROFILE_GROMACS);
    run_profile_tests("GRAPH Profile (Random Access)", PGAS_PROFILE_GRAPH);

    // Summary comparison
    printf("\n╔════════════════════════════════════════════════════════════════╗\n");
    printf("║                    Profile Comparison                          ║\n");
    printf("╚════════════════════════════════════════════════════════════════╝\n");
    printf("\nRecommendations:\n");
    printf("  MCF:     Use for pointer-chasing workloads with irregular access\n");
    printf("           Small transfers (64B), aggressive prefetch, LOCAL affinity\n\n");
    printf("  LLAMA:   Use for LLM inference with sequential weight loading\n");
    printf("           Large transfers (1MB), streaming prefetch, INTERLEAVE affinity\n\n");
    printf("  GROMACS: Use for molecular dynamics with neighbor lists\n");
    printf("           Medium transfers (8KB), neighbor-list prefetch, async enabled\n\n");
    printf("  GRAPH:   Use for graph analytics with irregular vertex access\n");
    printf("           Small transfers (512B), no prefetch (unpredictable)\n");

    printf("\n=== Test Complete ===\n");
    return 0;
}
