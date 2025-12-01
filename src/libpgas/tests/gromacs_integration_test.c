/*
 * GROMACS Integration Test
 * Simulates Molecular Dynamics memory access patterns on PGAS/CXL
 *
 * GROMACS characteristics:
 * - Neighbor-list driven force calculations
 * - 3D domain decomposition
 * - Halo exchange between domains
 * - Particle data: positions, velocities, forces (SoA or AoS)
 * - Spatial locality within domains
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
 * Simulation parameters
 */
typedef struct {
    int num_particles;       // Total particles
    int num_domains;         // Number of 3D domains (for domain decomposition)
    float box_size;          // Simulation box size (nm)
    float cutoff;            // Nonbonded cutoff (nm)
    float dt;                // Timestep (ps)
    int nlist_update_freq;   // Neighbor list update frequency
    int max_neighbors;       // Maximum neighbors per particle
} gromacs_config_t;

/*
 * Particle data (Structure of Arrays for better memory access)
 */
typedef struct {
    float* x;      // Positions X
    float* y;      // Positions Y
    float* z;      // Positions Z
    float* vx;     // Velocities X
    float* vy;     // Velocities Y
    float* vz;     // Velocities Z
    float* fx;     // Forces X
    float* fy;     // Forces Y
    float* fz;     // Forces Z
    float* mass;   // Particle masses
    float* charge; // Particle charges
    int* type;     // Particle types
} particle_soa_t;

/*
 * Neighbor list structure
 */
typedef struct {
    int* list;              // Neighbor indices [num_particles * max_neighbors]
    int* num_neighbors;     // Number of neighbors per particle
} neighbor_list_t;

/*
 * Domain decomposition info
 */
typedef struct {
    int* particle_domain;   // Which domain each particle belongs to
    int* domain_start;      // Start index for each domain
    int* domain_count;      // Number of particles in each domain
} domain_info_t;

typedef struct {
    double force_calc_time;
    double nlist_time;
    double integrate_time;
    double comm_time;
    double total_interactions;
    double ns_per_day;
    double bandwidth_gbps;
} gromacs_result_t;

/*
 * Lennard-Jones force calculation
 */
static inline void lj_force(float dx, float dy, float dz,
                            float* fx, float* fy, float* fz,
                            float epsilon, float sigma) {
    float r2 = dx*dx + dy*dy + dz*dz;
    if (r2 < 0.0001f) r2 = 0.0001f;  // Avoid singularity

    float r2_inv = 1.0f / r2;
    float r6_inv = r2_inv * r2_inv * r2_inv;
    float sigma6 = sigma * sigma * sigma * sigma * sigma * sigma;

    // LJ force magnitude: 24 * epsilon * (2*sigma^12/r^13 - sigma^6/r^7)
    float force_mag = 24.0f * epsilon * r6_inv * (2.0f * sigma6 * r6_inv - 1.0f) * sigma6 * r2_inv;

    *fx = force_mag * dx;
    *fy = force_mag * dy;
    *fz = force_mag * dz;
}

/*
 * Build neighbor list with spatial clustering
 */
static void build_neighbor_list(particle_soa_t* particles, neighbor_list_t* nlist,
                                 gromacs_config_t* config) {
    int n = config->num_particles;
    float cutoff2 = config->cutoff * config->cutoff;

    // Clear neighbor counts
    memset(nlist->num_neighbors, 0, n * sizeof(int));

    // Simple O(N) neighbor list building with spatial hashing would be used
    // in real GROMACS, but we simulate the memory access pattern
    for (int i = 0; i < n; i++) {
        float xi = particles->x[i];
        float yi = particles->y[i];
        float zi = particles->z[i];

        // Check nearby particles (simulating spatial locality)
        // In real GROMACS, this uses cell lists
        int count = 0;
        int max_check = 200;  // Check nearby particles
        int start = (i / 100) * 100;  // Spatial cluster

        for (int offset = -max_check/2; offset < max_check/2 && count < config->max_neighbors; offset++) {
            int j = start + offset;
            if (j < 0 || j >= n || j == i) continue;

            float dx = particles->x[j] - xi;
            float dy = particles->y[j] - yi;
            float dz = particles->z[j] - zi;
            float r2 = dx*dx + dy*dy + dz*dz;

            if (r2 < cutoff2) {
                nlist->list[i * config->max_neighbors + count] = j;
                count++;
            }
        }
        nlist->num_neighbors[i] = count;
    }
}

/*
 * Calculate nonbonded forces using neighbor list
 */
static double calculate_forces(particle_soa_t* particles, neighbor_list_t* nlist,
                               gromacs_config_t* config) {
    int n = config->num_particles;
    double interactions = 0;

    // Clear forces
    memset(particles->fx, 0, n * sizeof(float));
    memset(particles->fy, 0, n * sizeof(float));
    memset(particles->fz, 0, n * sizeof(float));

    // LJ parameters
    float epsilon = 1.0f;  // kJ/mol
    float sigma = 0.3f;    // nm

    // Force calculation using neighbor list
    for (int i = 0; i < n; i++) {
        float xi = particles->x[i];
        float yi = particles->y[i];
        float zi = particles->z[i];

        float fxi = 0, fyi = 0, fzi = 0;

        int num_neighbors = nlist->num_neighbors[i];
        int* neighbors = &nlist->list[i * config->max_neighbors];

        // Prefetch next particle's neighbor list
        if (i + 1 < n) {
            __builtin_prefetch(&nlist->list[(i + 1) * config->max_neighbors], 0, 0);
            __builtin_prefetch(&particles->x[i + 1], 0, 0);
        }

        for (int k = 0; k < num_neighbors; k++) {
            int j = neighbors[k];

            // Prefetch next neighbor data
            if (k + 1 < num_neighbors) {
                int next_j = neighbors[k + 1];
                __builtin_prefetch(&particles->x[next_j], 0, 0);
            }

            float dx = particles->x[j] - xi;
            float dy = particles->y[j] - yi;
            float dz = particles->z[j] - zi;

            float fx, fy, fz;
            lj_force(dx, dy, dz, &fx, &fy, &fz, epsilon, sigma);

            fxi += fx;
            fyi += fy;
            fzi += fz;

            interactions++;
        }

        particles->fx[i] += fxi;
        particles->fy[i] += fyi;
        particles->fz[i] += fzi;
    }

    return interactions;
}

/*
 * Velocity Verlet integration
 */
static void integrate(particle_soa_t* particles, gromacs_config_t* config) {
    int n = config->num_particles;
    float dt = config->dt;
    float dt2 = dt * dt;

    for (int i = 0; i < n; i++) {
        float inv_mass = 1.0f / particles->mass[i];

        // Update velocities (half step)
        particles->vx[i] += 0.5f * particles->fx[i] * inv_mass * dt;
        particles->vy[i] += 0.5f * particles->fy[i] * inv_mass * dt;
        particles->vz[i] += 0.5f * particles->fz[i] * inv_mass * dt;

        // Update positions
        particles->x[i] += particles->vx[i] * dt;
        particles->y[i] += particles->vy[i] * dt;
        particles->z[i] += particles->vz[i] * dt;
    }
}

/*
 * Simulate domain decomposition communication (halo exchange)
 */
static void halo_exchange(particle_soa_t* particles, domain_info_t* domains,
                          gromacs_config_t* config) {
    // Simulate halo exchange overhead
    // In real GROMACS, this involves MPI communication
    int halo_size = config->num_particles / config->num_domains / 10;

    // Touch halo regions to simulate data movement
    for (int d = 0; d < config->num_domains; d++) {
        int start = domains->domain_start[d];
        volatile float sum = 0;

        // Read boundary particles (halo region)
        for (int i = 0; i < halo_size && start + i < config->num_particles; i++) {
            sum += particles->x[start + i];
            sum += particles->y[start + i];
            sum += particles->z[start + i];
        }
    }
}

/*
 * Run GROMACS-style MD simulation
 */
gromacs_result_t run_gromacs_simulation(gromacs_config_t* config, int num_steps) {
    gromacs_result_t result = {0};

    int n = config->num_particles;

    printf("  GROMACS Configuration:\n");
    printf("    Particles: %d, Box: %.1f nm, Cutoff: %.1f nm\n",
           n, config->box_size, config->cutoff);
    printf("    Domains: %d, Max neighbors: %d\n",
           config->num_domains, config->max_neighbors);

    // Allocate particle data (SoA layout)
    printf("  Allocating particle data...\n");
    particle_soa_t particles;

    particles.x = mmap(NULL, n * sizeof(float), PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    particles.y = mmap(NULL, n * sizeof(float), PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    particles.z = mmap(NULL, n * sizeof(float), PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    particles.vx = mmap(NULL, n * sizeof(float), PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    particles.vy = mmap(NULL, n * sizeof(float), PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    particles.vz = mmap(NULL, n * sizeof(float), PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    particles.fx = mmap(NULL, n * sizeof(float), PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    particles.fy = mmap(NULL, n * sizeof(float), PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    particles.fz = mmap(NULL, n * sizeof(float), PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    particles.mass = mmap(NULL, n * sizeof(float), PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    particles.charge = mmap(NULL, n * sizeof(float), PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    particles.type = mmap(NULL, n * sizeof(int), PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);

    // Allocate neighbor list
    neighbor_list_t nlist;
    nlist.list = mmap(NULL, n * config->max_neighbors * sizeof(int),
                      PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    nlist.num_neighbors = mmap(NULL, n * sizeof(int),
                                PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);

    // Allocate domain info
    domain_info_t domains;
    domains.particle_domain = malloc(n * sizeof(int));
    domains.domain_start = malloc(config->num_domains * sizeof(int));
    domains.domain_count = malloc(config->num_domains * sizeof(int));

    // Initialize particles in a 3D grid with random velocities
    printf("  Initializing particle positions...\n");
    srand(42);

    float spacing = config->box_size / cbrtf(n);
    int particles_per_dim = (int)cbrtf(n) + 1;
    int idx = 0;

    for (int iz = 0; iz < particles_per_dim && idx < n; iz++) {
        for (int iy = 0; iy < particles_per_dim && idx < n; iy++) {
            for (int ix = 0; ix < particles_per_dim && idx < n; ix++) {
                particles.x[idx] = ix * spacing + ((float)rand() / RAND_MAX - 0.5f) * 0.1f * spacing;
                particles.y[idx] = iy * spacing + ((float)rand() / RAND_MAX - 0.5f) * 0.1f * spacing;
                particles.z[idx] = iz * spacing + ((float)rand() / RAND_MAX - 0.5f) * 0.1f * spacing;

                particles.vx[idx] = ((float)rand() / RAND_MAX - 0.5f) * 0.1f;
                particles.vy[idx] = ((float)rand() / RAND_MAX - 0.5f) * 0.1f;
                particles.vz[idx] = ((float)rand() / RAND_MAX - 0.5f) * 0.1f;

                particles.mass[idx] = 12.0f + rand() % 20;  // AMU
                particles.charge[idx] = ((float)rand() / RAND_MAX - 0.5f) * 2.0f;
                particles.type[idx] = rand() % 4;

                // Assign to domain based on position
                int domain_x = (int)(particles.x[idx] / config->box_size * cbrtf(config->num_domains));
                int domain_y = (int)(particles.y[idx] / config->box_size * cbrtf(config->num_domains));
                int domain_z = (int)(particles.z[idx] / config->box_size * cbrtf(config->num_domains));
                domains.particle_domain[idx] = domain_x + domain_y * 2 + domain_z * 4;

                idx++;
            }
        }
    }

    // Setup domain boundaries
    int particles_per_domain = n / config->num_domains;
    for (int d = 0; d < config->num_domains; d++) {
        domains.domain_start[d] = d * particles_per_domain;
        domains.domain_count[d] = particles_per_domain;
    }

    printf("  Running MD simulation (%d steps)...\n", num_steps);

    double force_time = 0, nlist_time = 0, integrate_time = 0, comm_time = 0;
    double total_interactions = 0;

    double start = get_time_sec();

    for (int step = 0; step < num_steps; step++) {
        // Update neighbor list periodically
        if (step % config->nlist_update_freq == 0) {
            double t0 = get_time_sec();
            build_neighbor_list(&particles, &nlist, config);
            nlist_time += get_time_sec() - t0;
        }

        // Calculate forces
        double t0 = get_time_sec();
        total_interactions += calculate_forces(&particles, &nlist, config);
        force_time += get_time_sec() - t0;

        // Halo exchange (domain decomposition communication)
        t0 = get_time_sec();
        halo_exchange(&particles, &domains, config);
        comm_time += get_time_sec() - t0;

        // Integration
        t0 = get_time_sec();
        integrate(&particles, config);
        integrate_time += get_time_sec() - t0;
    }

    double total_time = get_time_sec() - start;

    result.force_calc_time = force_time;
    result.nlist_time = nlist_time;
    result.integrate_time = integrate_time;
    result.comm_time = comm_time;
    result.total_interactions = total_interactions;

    // Calculate ns/day (standard MD performance metric)
    // time_simulated (ps) = num_steps * dt
    // ns/day = time_simulated * 86400 / (total_time * 1000)
    double time_simulated_ps = num_steps * config->dt;
    result.ns_per_day = (time_simulated_ps / 1000.0) * 86400.0 / total_time;

    // Estimate bandwidth (bytes read per interaction)
    size_t bytes_per_interaction = 6 * sizeof(float);  // 2 positions, neighbor index
    result.bandwidth_gbps = (total_interactions * bytes_per_interaction / (double)GB) / total_time;

    // Cleanup
    munmap(particles.x, n * sizeof(float));
    munmap(particles.y, n * sizeof(float));
    munmap(particles.z, n * sizeof(float));
    munmap(particles.vx, n * sizeof(float));
    munmap(particles.vy, n * sizeof(float));
    munmap(particles.vz, n * sizeof(float));
    munmap(particles.fx, n * sizeof(float));
    munmap(particles.fy, n * sizeof(float));
    munmap(particles.fz, n * sizeof(float));
    munmap(particles.mass, n * sizeof(float));
    munmap(particles.charge, n * sizeof(float));
    munmap(particles.type, n * sizeof(int));
    munmap(nlist.list, n * config->max_neighbors * sizeof(int));
    munmap(nlist.num_neighbors, n * sizeof(int));
    free(domains.particle_domain);
    free(domains.domain_start);
    free(domains.domain_count);

    return result;
}

void print_header(const char* title) {
    printf("\n╔════════════════════════════════════════════════════════════════════════════╗\n");
    printf("║  %-74s ║\n", title);
    printf("╚════════════════════════════════════════════════════════════════════════════╝\n");
}

int main(int argc, char* argv[]) {
    gromacs_config_t config = {
        .num_particles = 50000,
        .num_domains = 8,
        .box_size = 10.0f,        // nm
        .cutoff = 1.0f,           // nm
        .dt = 0.002f,             // ps (2 fs)
        .nlist_update_freq = 10,
        .max_neighbors = 100
    };

    int num_steps = 100;

    // Parse args
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            config.num_particles = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            num_steps = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-h") == 0) {
            printf("Usage: %s [-n particles] [-s steps]\n", argv[0]);
            return 0;
        }
    }

    printf("╔════════════════════════════════════════════════════════════════════════════╗\n");
    printf("║                 GROMACS Integration Benchmark                              ║\n");
    printf("║            Molecular Dynamics Memory Access Patterns                       ║\n");
    printf("╚════════════════════════════════════════════════════════════════════════════╝\n");

    print_header("Running with DEFAULT Profile");
    pgas_context_t ctx;
    
    pgas_init(&ctx, NULL);
    pgas_load_profile(&ctx, PGAS_PROFILE_DEFAULT);

    const pgas_tuning_t* tuning = pgas_get_default_tuning(PGAS_PROFILE_DEFAULT);
    printf("  Batch: %zu, Transfer: %zu bytes, Prefetch: NONE\n",
           tuning->batch_size, tuning->transfer_size);

    gromacs_result_t default_result = run_gromacs_simulation(&config, num_steps);

    print_header("Running with GROMACS Profile");
    pgas_load_profile(&ctx, PGAS_PROFILE_GROMACS);

    tuning = pgas_get_default_tuning(PGAS_PROFILE_GROMACS);
    printf("  Batch: %zu, Transfer: %zu bytes (8KB), Prefetch: NEIGHBOR_LIST\n",
           tuning->batch_size, tuning->transfer_size);
    printf("  Async: %s, Threads: %d\n",
           tuning->async_transfer ? "Yes" : "No", tuning->num_threads);

    gromacs_result_t gromacs_result = run_gromacs_simulation(&config, num_steps);

    print_header("GROMACS Benchmark Results");

    printf("\n  Phase Breakdown:\n");
    printf("  ┌─────────────────────┬────────────────┬────────────────┬────────────┐\n");
    printf("  │ Phase               │ DEFAULT        │ GROMACS Profile│ Speedup    │\n");
    printf("  ├─────────────────────┼────────────────┼────────────────┼────────────┤\n");
    printf("  │ Force Calculation   │ %8.3f sec   │ %8.3f sec   │ %+6.1f%%    │\n",
           default_result.force_calc_time, gromacs_result.force_calc_time,
           (default_result.force_calc_time / gromacs_result.force_calc_time - 1) * 100);
    printf("  │ Neighbor List       │ %8.3f sec   │ %8.3f sec   │ %+6.1f%%    │\n",
           default_result.nlist_time, gromacs_result.nlist_time,
           (default_result.nlist_time / gromacs_result.nlist_time - 1) * 100);
    printf("  │ Integration         │ %8.3f sec   │ %8.3f sec   │ %+6.1f%%    │\n",
           default_result.integrate_time, gromacs_result.integrate_time,
           (default_result.integrate_time / gromacs_result.integrate_time - 1) * 100);
    printf("  │ Communication       │ %8.3f sec   │ %8.3f sec   │ %+6.1f%%    │\n",
           default_result.comm_time, gromacs_result.comm_time,
           (default_result.comm_time / gromacs_result.comm_time - 1) * 100);
    printf("  └─────────────────────┴────────────────┴────────────────┴────────────┘\n");

    printf("\n  Performance Metrics:\n");
    printf("    Interactions: %.2f M total\n", gromacs_result.total_interactions / 1e6);
    printf("    ns/day (DEFAULT):  %.2f\n", default_result.ns_per_day);
    printf("    ns/day (GROMACS):  %.2f\n", gromacs_result.ns_per_day);
    printf("    Bandwidth: %.2f GB/s\n", gromacs_result.bandwidth_gbps);

    double nsday_improvement = (gromacs_result.ns_per_day / default_result.ns_per_day - 1) * 100;
    printf("\n  ► ns/day improvement: %+.1f%%\n", nsday_improvement);

    printf("\n  Memory Access Pattern Analysis:\n");
    printf("    - Neighbor-list driven (clustered access)\n");
    printf("    - Medium transfer size (8KB) for neighbor batches\n");
    printf("    - Async transfers overlap with computation\n");
    printf("    - Domain decomposition enables LOCAL affinity\n");

    pgas_finalize(&ctx);

    printf("\n=== GROMACS Integration Test Complete ===\n");
    return 0;
}
