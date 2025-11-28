/*
 * CXL Memory Self-Loop Test
 * Tests graph algorithms using CXL memory on NUMA node 2
 *
 * A self-loop graph is a graph where each vertex has an edge to itself.
 * This is a simple test case to verify CXL memory allocation and access.
 */

#include <iostream>
#include <vector>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <numa.h>
#include <numaif.h>
#include <omp.h>

// Configuration
#define CXL_DAX_DEVICE "/dev/dax0.0"  // NUMA 2
#define CXL_NUMA_NODE 2
#define DEFAULT_NUM_VERTICES 1000000
#define DEFAULT_MEMORY_SIZE (1ULL << 30)  // 1GB

// Timer helper
class Timer {
public:
    void start() { start_ = std::chrono::high_resolution_clock::now(); }
    double elapsed_ms() {
        auto end = std::chrono::high_resolution_clock::now();
        return std::chrono::duration<double, std::milli>(end - start_).count();
    }
    double elapsed_sec() { return elapsed_ms() / 1000.0; }
private:
    std::chrono::time_point<std::chrono::high_resolution_clock> start_;
};

// CXL Memory Manager for NUMA 2
class CXLMemoryManager {
public:
    CXLMemoryManager() : base_ptr_(nullptr), size_(0), dax_fd_(-1), use_dax_(false) {}

    ~CXLMemoryManager() {
        if (base_ptr_) {
            munmap(base_ptr_, size_);
        }
        if (dax_fd_ >= 0) {
            close(dax_fd_);
        }
    }

    bool init(size_t size, bool try_dax = true) {
        size_ = size;

        if (try_dax) {
            // Try DAX device first
            dax_fd_ = open(CXL_DAX_DEVICE, O_RDWR);
            if (dax_fd_ >= 0) {
                base_ptr_ = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                                 MAP_SHARED, dax_fd_, 0);
                if (base_ptr_ != MAP_FAILED) {
                    use_dax_ = true;
                    std::cout << "  Using DAX device: " << CXL_DAX_DEVICE << std::endl;
                    return true;
                }
                close(dax_fd_);
                dax_fd_ = -1;
            }
            std::cout << "  DAX not available, falling back to NUMA allocation" << std::endl;
        }

        // Fallback: allocate on NUMA node 2
        base_ptr_ = numa_alloc_onnode(size, CXL_NUMA_NODE);
        if (base_ptr_ == nullptr) {
            // Final fallback: regular mmap
            std::cout << "  NUMA alloc failed, using anonymous mmap" << std::endl;
            base_ptr_ = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (base_ptr_ == MAP_FAILED) {
                base_ptr_ = nullptr;
                return false;
            }
            // Move to NUMA 2
            unsigned long nodemask = 1UL << CXL_NUMA_NODE;
            mbind(base_ptr_, size, MPOL_BIND, &nodemask, sizeof(nodemask) * 8, MPOL_MF_MOVE);
        }

        return base_ptr_ != nullptr;
    }

    void* get_base() { return base_ptr_; }
    size_t get_size() { return size_; }
    bool is_dax() { return use_dax_; }

    // Verify memory is on correct NUMA node
    int get_actual_numa_node() {
        if (!base_ptr_) return -1;
        int status;
        void* ptr = base_ptr_;
        if (get_mempolicy(&status, nullptr, 0, ptr, MPOL_F_NODE | MPOL_F_ADDR) == 0) {
            return status;
        }
        return -1;
    }

    // Flush cache line
    void flush(void* addr, size_t size) {
        char* p = (char*)addr;
        for (size_t i = 0; i < size; i += 64) {
            __asm__ volatile("clflushopt (%0)" :: "r"(p + i) : "memory");
        }
        __asm__ volatile("sfence" ::: "memory");
    }

private:
    void* base_ptr_;
    size_t size_;
    int dax_fd_;
    bool use_dax_;
};

// Simple CSR Graph on CXL memory
class CXLGraph {
public:
    CXLGraph(CXLMemoryManager& mem) : mem_(mem), num_vertices_(0), num_edges_(0) {}

    // Build self-loop graph: each vertex has edge to itself
    void build_selfloop(int64_t num_vertices) {
        num_vertices_ = num_vertices;
        num_edges_ = num_vertices;  // One edge per vertex

        char* base = (char*)mem_.get_base();
        size_t offset = 0;

        // Allocate index array (num_vertices + 1 entries)
        index_ = (int64_t*)(base + offset);
        offset += (num_vertices + 1) * sizeof(int64_t);

        // Allocate neighbors array (num_edges entries)
        neighbors_ = (int64_t*)(base + offset);
        offset += num_edges_ * sizeof(int64_t);

        // Allocate vertex data array (for algorithms)
        vertex_data_ = (int64_t*)(base + offset);
        offset += num_vertices * sizeof(int64_t);

        std::cout << "  Graph memory layout:" << std::endl;
        std::cout << "    Index array: " << (num_vertices + 1) * sizeof(int64_t) / 1024 << " KB" << std::endl;
        std::cout << "    Neighbors: " << num_edges_ * sizeof(int64_t) / 1024 << " KB" << std::endl;
        std::cout << "    Vertex data: " << num_vertices * sizeof(int64_t) / 1024 << " KB" << std::endl;
        std::cout << "    Total: " << offset / (1024 * 1024) << " MB" << std::endl;

        // Build CSR structure: each vertex v has one neighbor (itself)
        Timer t;
        t.start();

        #pragma omp parallel for
        for (int64_t v = 0; v < num_vertices; v++) {
            index_[v] = v;           // Edge starts at position v
            neighbors_[v] = v;       // Self-loop: neighbor is itself
            vertex_data_[v] = -1;    // Initialize vertex data
        }
        index_[num_vertices] = num_edges_;

        // Flush to CXL memory
        mem_.flush(index_, (num_vertices + 1) * sizeof(int64_t));
        mem_.flush(neighbors_, num_edges_ * sizeof(int64_t));
        mem_.flush(vertex_data_, num_vertices * sizeof(int64_t));

        std::cout << "  Build time: " << t.elapsed_ms() << " ms" << std::endl;
    }

    // Build graph with self-loops plus random edges
    void build_selfloop_plus_random(int64_t num_vertices, int edges_per_vertex) {
        num_vertices_ = num_vertices;
        num_edges_ = num_vertices * edges_per_vertex;

        char* base = (char*)mem_.get_base();
        size_t offset = 0;

        index_ = (int64_t*)(base + offset);
        offset += (num_vertices + 1) * sizeof(int64_t);

        neighbors_ = (int64_t*)(base + offset);
        offset += num_edges_ * sizeof(int64_t);

        vertex_data_ = (int64_t*)(base + offset);
        offset += num_vertices * sizeof(int64_t);

        Timer t;
        t.start();

        #pragma omp parallel for
        for (int64_t v = 0; v < num_vertices; v++) {
            index_[v] = v * edges_per_vertex;
            // First edge is self-loop
            neighbors_[v * edges_per_vertex] = v;
            // Additional random edges
            unsigned int seed = v;
            for (int e = 1; e < edges_per_vertex; e++) {
                neighbors_[v * edges_per_vertex + e] = rand_r(&seed) % num_vertices;
            }
            vertex_data_[v] = -1;
        }
        index_[num_vertices] = num_edges_;

        mem_.flush(index_, (num_vertices + 1) * sizeof(int64_t));
        mem_.flush(neighbors_, num_edges_ * sizeof(int64_t));

        std::cout << "  Build time: " << t.elapsed_ms() << " ms" << std::endl;
    }

    int64_t num_vertices() const { return num_vertices_; }
    int64_t num_edges() const { return num_edges_; }
    int64_t out_degree(int64_t v) const { return index_[v + 1] - index_[v]; }
    int64_t* neighbors(int64_t v) { return &neighbors_[index_[v]]; }
    int64_t* vertex_data() { return vertex_data_; }
    int64_t* index_array() { return index_; }

private:
    CXLMemoryManager& mem_;
    int64_t num_vertices_;
    int64_t num_edges_;
    int64_t* index_;
    int64_t* neighbors_;
    int64_t* vertex_data_;
};

// BFS on CXL graph
int64_t run_bfs(CXLGraph& graph, int64_t source) {
    int64_t num_vertices = graph.num_vertices();
    int64_t* parent = graph.vertex_data();

    // Initialize
    #pragma omp parallel for
    for (int64_t v = 0; v < num_vertices; v++) {
        parent[v] = -1;
    }
    parent[source] = source;

    std::vector<int64_t> frontier;
    std::vector<int64_t> next_frontier;
    frontier.push_back(source);

    int64_t depth = 0;
    int64_t visited = 1;

    while (!frontier.empty()) {
        next_frontier.clear();

        #pragma omp parallel
        {
            std::vector<int64_t> local_next;

            #pragma omp for nowait
            for (size_t i = 0; i < frontier.size(); i++) {
                int64_t u = frontier[i];
                int64_t degree = graph.out_degree(u);
                int64_t* neighs = graph.neighbors(u);

                for (int64_t j = 0; j < degree; j++) {
                    int64_t v = neighs[j];
                    if (parent[v] == -1) {
                        int64_t expected = -1;
                        if (__sync_bool_compare_and_swap(&parent[v], expected, u)) {
                            local_next.push_back(v);
                        }
                    }
                }
            }

            #pragma omp critical
            {
                next_frontier.insert(next_frontier.end(),
                                    local_next.begin(), local_next.end());
            }
        }

        visited += next_frontier.size();
        frontier = std::move(next_frontier);
        depth++;
    }

    return visited;
}

// PageRank on CXL graph
void run_pagerank(CXLGraph& graph, int max_iters, double damping = 0.85) {
    int64_t num_vertices = graph.num_vertices();

    // Allocate scores in main memory (we'll copy to CXL for comparison)
    std::vector<double> scores(num_vertices, 1.0 / num_vertices);
    std::vector<double> new_scores(num_vertices, 0);

    double base_score = (1.0 - damping) / num_vertices;

    for (int iter = 0; iter < max_iters; iter++) {
        #pragma omp parallel for
        for (int64_t v = 0; v < num_vertices; v++) {
            double sum = 0;
            int64_t degree = graph.out_degree(v);
            int64_t* neighs = graph.neighbors(v);

            for (int64_t j = 0; j < degree; j++) {
                int64_t u = neighs[j];
                int64_t u_degree = graph.out_degree(u);
                sum += scores[u] / (u_degree > 0 ? u_degree : 1);
            }

            new_scores[v] = base_score + damping * sum;
        }

        std::swap(scores, new_scores);
    }

    // For self-loop graph, all scores should be equal
    double expected = 1.0 / num_vertices;
    double max_diff = 0;
    for (int64_t v = 0; v < std::min((int64_t)10, num_vertices); v++) {
        max_diff = std::max(max_diff, std::abs(scores[v] - expected));
    }
    std::cout << "  PageRank max diff from expected: " << max_diff << std::endl;
}

// Memory bandwidth test
void run_bandwidth_test(CXLMemoryManager& mem, size_t test_size) {
    char* base = (char*)mem.get_base();

    // Sequential write
    Timer t;
    t.start();

    #pragma omp parallel for
    for (size_t i = 0; i < test_size; i += 64) {
        *(volatile int64_t*)(base + i) = i;
    }

    double write_time = t.elapsed_sec();
    double write_bw = (test_size / (1024.0 * 1024 * 1024)) / write_time;

    // Sequential read
    t.start();
    volatile int64_t sum = 0;

    #pragma omp parallel for reduction(+:sum)
    for (size_t i = 0; i < test_size; i += 64) {
        sum += *(volatile int64_t*)(base + i);
    }

    double read_time = t.elapsed_sec();
    double read_bw = (test_size / (1024.0 * 1024 * 1024)) / read_time;

    std::cout << "  Sequential Write: " << write_bw << " GB/s" << std::endl;
    std::cout << "  Sequential Read: " << read_bw << " GB/s" << std::endl;
}

// Latency test
void run_latency_test(CXLMemoryManager& mem) {
    int64_t* base = (int64_t*)mem.get_base();
    size_t num_accesses = 1000000;
    size_t stride = 4096 / sizeof(int64_t);  // Page stride

    // Pointer chasing for latency measurement
    Timer t;
    t.start();

    volatile int64_t val = 0;
    size_t idx = 0;
    for (size_t i = 0; i < num_accesses; i++) {
        val = base[idx];
        idx = (idx + stride) % (mem.get_size() / sizeof(int64_t));
    }

    double total_time = t.elapsed_ms() * 1000000;  // Convert to ns
    double avg_latency = total_time / num_accesses;

    std::cout << "  Random Access Latency: " << avg_latency << " ns" << std::endl;
    (void)val;  // Prevent optimization
}

void print_usage(const char* prog) {
    std::cout << "Usage: " << prog << " [options]\n\n";
    std::cout << "CXL Memory Self-Loop Graph Test\n\n";
    std::cout << "Options:\n";
    std::cout << "  -n, --vertices N    Number of vertices (default: 1M)\n";
    std::cout << "  -e, --edges N       Edges per vertex (default: 1 = self-loop only)\n";
    std::cout << "  -m, --memory SIZE   CXL memory size in GB (default: 1)\n";
    std::cout << "  -t, --threads N     Number of OpenMP threads\n";
    std::cout << "  --bfs               Run BFS test\n";
    std::cout << "  --pagerank          Run PageRank test\n";
    std::cout << "  --bandwidth         Run bandwidth test\n";
    std::cout << "  --latency           Run latency test\n";
    std::cout << "  --all               Run all tests\n";
    std::cout << "  -h, --help          Show this help\n";
}

int main(int argc, char* argv[]) {
    // Default parameters
    int64_t num_vertices = DEFAULT_NUM_VERTICES;
    int edges_per_vertex = 1;
    size_t memory_gb = 1;
    int num_threads = omp_get_max_threads();
    bool run_bfs_test = false;
    bool run_pr_test = false;
    bool run_bw_test = false;
    bool run_lat_test = false;

    // Parse arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0 || strcmp(argv[i], "--vertices") == 0) {
            num_vertices = std::stoll(argv[++i]);
        } else if (strcmp(argv[i], "-e") == 0 || strcmp(argv[i], "--edges") == 0) {
            edges_per_vertex = std::stoi(argv[++i]);
        } else if (strcmp(argv[i], "-m") == 0 || strcmp(argv[i], "--memory") == 0) {
            memory_gb = std::stoul(argv[++i]);
        } else if (strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--threads") == 0) {
            num_threads = std::stoi(argv[++i]);
        } else if (strcmp(argv[i], "--bfs") == 0) {
            run_bfs_test = true;
        } else if (strcmp(argv[i], "--pagerank") == 0) {
            run_pr_test = true;
        } else if (strcmp(argv[i], "--bandwidth") == 0) {
            run_bw_test = true;
        } else if (strcmp(argv[i], "--latency") == 0) {
            run_lat_test = true;
        } else if (strcmp(argv[i], "--all") == 0) {
            run_bfs_test = run_pr_test = run_bw_test = run_lat_test = true;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
    }

    // Default to running all tests
    if (!run_bfs_test && !run_pr_test && !run_bw_test && !run_lat_test) {
        run_bfs_test = run_pr_test = run_bw_test = run_lat_test = true;
    }

    omp_set_num_threads(num_threads);

    std::cout << "╔════════════════════════════════════════════════════════════════╗\n";
    std::cout << "║        CXL Memory Self-Loop Graph Test (NUMA Node 2)           ║\n";
    std::cout << "╚════════════════════════════════════════════════════════════════╝\n\n";

    std::cout << "Configuration:\n";
    std::cout << "  Vertices: " << num_vertices << std::endl;
    std::cout << "  Edges per vertex: " << edges_per_vertex << std::endl;
    std::cout << "  CXL Memory: " << memory_gb << " GB" << std::endl;
    std::cout << "  OpenMP threads: " << num_threads << std::endl;
    std::cout << "  Target NUMA node: " << CXL_NUMA_NODE << std::endl;
    std::cout << std::endl;

    // Initialize CXL memory
    std::cout << "Initializing CXL memory on NUMA " << CXL_NUMA_NODE << "...\n";
    CXLMemoryManager mem;
    if (!mem.init(memory_gb * 1024ULL * 1024 * 1024)) {
        std::cerr << "Failed to initialize CXL memory!" << std::endl;
        return 1;
    }

    int actual_node = mem.get_actual_numa_node();
    std::cout << "  Memory allocated at: " << mem.get_base() << std::endl;
    std::cout << "  Actual NUMA node: " << actual_node << std::endl;
    std::cout << "  DAX mode: " << (mem.is_dax() ? "yes" : "no") << std::endl;
    std::cout << std::endl;

    // Run bandwidth test
    if (run_bw_test) {
        std::cout << "=== CXL Memory Bandwidth Test ===" << std::endl;
        size_t bw_test_size = memory_gb * 1024ULL * 1024 * 1024;
        if (bw_test_size > 512 * 1024 * 1024) bw_test_size = 512 * 1024 * 1024;
        run_bandwidth_test(mem, bw_test_size);
        std::cout << std::endl;
    }

    // Run latency test
    if (run_lat_test) {
        std::cout << "=== CXL Memory Latency Test ===" << std::endl;
        run_latency_test(mem);
        std::cout << std::endl;
    }

    // Build graph
    std::cout << "=== Building Self-Loop Graph on CXL Memory ===" << std::endl;
    CXLGraph graph(mem);
    if (edges_per_vertex == 1) {
        graph.build_selfloop(num_vertices);
    } else {
        graph.build_selfloop_plus_random(num_vertices, edges_per_vertex);
    }
    std::cout << "  Vertices: " << graph.num_vertices() << std::endl;
    std::cout << "  Edges: " << graph.num_edges() << std::endl;
    std::cout << std::endl;

    // Run BFS
    if (run_bfs_test) {
        std::cout << "=== BFS Test ===" << std::endl;
        Timer t;
        t.start();
        int64_t visited = run_bfs(graph, 0);
        double bfs_time = t.elapsed_ms();

        std::cout << "  Source: 0" << std::endl;
        std::cout << "  Visited: " << visited << " / " << num_vertices << std::endl;
        std::cout << "  Time: " << bfs_time << " ms" << std::endl;
        std::cout << "  MTEPS: " << (graph.num_edges() / 1e6) / (bfs_time / 1000) << std::endl;

        // Verify: for self-loop graph, should visit all vertices
        if (visited == num_vertices) {
            std::cout << "  Verification: PASSED ✓" << std::endl;
        } else {
            std::cout << "  Verification: FAILED ✗ (expected " << num_vertices << ")" << std::endl;
        }
        std::cout << std::endl;
    }

    // Run PageRank
    if (run_pr_test) {
        std::cout << "=== PageRank Test ===" << std::endl;
        Timer t;
        t.start();
        run_pagerank(graph, 10);
        double pr_time = t.elapsed_ms();

        std::cout << "  Iterations: 10" << std::endl;
        std::cout << "  Time: " << pr_time << " ms" << std::endl;
        std::cout << std::endl;
    }

    std::cout << "=== All Tests Complete ===" << std::endl;
    return 0;
}
