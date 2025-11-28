#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <chrono>
#include <getopt.h>

#include "pgas_graph.h"
#include "pgas_algorithms.h"

void print_usage(const char* prog) {
    std::cout << "Usage: " << prog << " [options]\n\n";
    std::cout << "GAPBS with CXL PGAS - Distributed Graph Analytics\n\n";
    std::cout << "Options:\n";
    std::cout << "  -c, --config FILE      PGAS configuration file (required)\n";
    std::cout << "  -g, --graph FILE       Graph file in edge list format\n";
    std::cout << "  -n, --nodes N          Number of nodes (for synthetic graphs)\n";
    std::cout << "  -e, --edges E          Number of edges (for synthetic graphs)\n";
    std::cout << "  -a, --algorithm ALG    Algorithm: bfs, pr, sssp, cc, tc (default: bfs)\n";
    std::cout << "  -s, --source N         Source vertex for BFS/SSSP (default: 0)\n";
    std::cout << "  -d, --directed         Treat graph as directed\n";
    std::cout << "  --pr-iters N           Max PageRank iterations (default: 100)\n";
    std::cout << "  --pr-epsilon E         PageRank convergence threshold (default: 1e-4)\n";
    std::cout << "  -v, --verify           Verify results\n";
    std::cout << "  -h, --help             Show this help\n";
    std::cout << "\nExamples:\n";
    std::cout << "  " << prog << " -c nodes.conf -g graph.el -a bfs -s 0\n";
    std::cout << "  " << prog << " -c nodes.conf -n 1000000 -e 10000000 -a pr\n";
}

// Generate synthetic graph (RMAT)
std::vector<std::pair<NodeID, NodeID>> generate_rmat_graph(
    NodeID num_nodes, EdgeID num_edges,
    double a = 0.57, double b = 0.19, double c = 0.19) {

    std::vector<std::pair<NodeID, NodeID>> edges;
    edges.reserve(num_edges);

    double d = 1.0 - a - b - c;

    for (EdgeID i = 0; i < num_edges; i++) {
        NodeID u = 0, v = 0;
        NodeID step = num_nodes / 2;

        while (step > 0) {
            double r = (double)rand() / RAND_MAX;

            if (r < a) {
                // Top-left quadrant
            } else if (r < a + b) {
                // Top-right quadrant
                v += step;
            } else if (r < a + b + c) {
                // Bottom-left quadrant
                u += step;
            } else {
                // Bottom-right quadrant
                u += step;
                v += step;
            }

            step /= 2;
        }

        if (u != v) {
            edges.emplace_back(u, v);
        }
    }

    return edges;
}

// Load graph from edge list file
std::vector<std::pair<NodeID, NodeID>> load_edge_list(
    const std::string& filename, NodeID& num_nodes) {

    std::vector<std::pair<NodeID, NodeID>> edges;
    std::ifstream file(filename);

    if (!file.is_open()) {
        std::cerr << "Error: Cannot open graph file: " << filename << std::endl;
        return edges;
    }

    std::string line;
    NodeID max_node = 0;

    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#' || line[0] == '%') continue;

        std::istringstream iss(line);
        NodeID u, v;
        if (iss >> u >> v) {
            edges.emplace_back(u, v);
            max_node = std::max(max_node, std::max(u, v));
        }
    }

    num_nodes = max_node + 1;
    return edges;
}

int main(int argc, char* argv[]) {
    // Default parameters
    std::string config_file;
    std::string graph_file;
    NodeID num_nodes = 0;
    EdgeID num_edges = 0;
    std::string algorithm = "bfs";
    NodeID source = 0;
    bool directed = false;
    int pr_max_iters = 100;
    double pr_epsilon = 1e-4;
    bool verify = false;

    // Parse command line
    static struct option long_options[] = {
        {"config", required_argument, 0, 'c'},
        {"graph", required_argument, 0, 'g'},
        {"nodes", required_argument, 0, 'n'},
        {"edges", required_argument, 0, 'e'},
        {"algorithm", required_argument, 0, 'a'},
        {"source", required_argument, 0, 's'},
        {"directed", no_argument, 0, 'd'},
        {"pr-iters", required_argument, 0, 'i'},
        {"pr-epsilon", required_argument, 0, 'p'},
        {"verify", no_argument, 0, 'v'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "c:g:n:e:a:s:di:p:vh", long_options, NULL)) != -1) {
        switch (opt) {
            case 'c': config_file = optarg; break;
            case 'g': graph_file = optarg; break;
            case 'n': num_nodes = std::stoll(optarg); break;
            case 'e': num_edges = std::stoll(optarg); break;
            case 'a': algorithm = optarg; break;
            case 's': source = std::stoll(optarg); break;
            case 'd': directed = true; break;
            case 'i': pr_max_iters = std::stoi(optarg); break;
            case 'p': pr_epsilon = std::stod(optarg); break;
            case 'v': verify = true; break;
            case 'h':
            default:
                print_usage(argv[0]);
                return (opt == 'h') ? 0 : 1;
        }
    }

    if (config_file.empty()) {
        std::cerr << "Error: Configuration file required\n\n";
        print_usage(argv[0]);
        return 1;
    }

    std::cout << "╔════════════════════════════════════════════════════════════════╗\n";
    std::cout << "║     GAPBS with CXL PGAS - Distributed Graph Analytics          ║\n";
    std::cout << "╚════════════════════════════════════════════════════════════════╝\n\n";

    // Initialize PGAS
    std::cout << "Initializing PGAS runtime...\n";
    pgas_context_t pgas_ctx;
    if (pgas_init(&pgas_ctx, config_file.c_str()) != 0) {
        std::cerr << "Failed to initialize PGAS\n";
        return 1;
    }

    std::cout << "  Local node: " << pgas_my_node(&pgas_ctx) << "\n";
    std::cout << "  Total nodes: " << pgas_num_nodes(&pgas_ctx) << "\n\n";

    // Load or generate graph
    std::vector<std::pair<NodeID, NodeID>> edges;

    if (!graph_file.empty()) {
        std::cout << "Loading graph from " << graph_file << "...\n";
        edges = load_edge_list(graph_file, num_nodes);
    } else if (num_nodes > 0 && num_edges > 0) {
        std::cout << "Generating RMAT graph: " << num_nodes << " nodes, "
                  << num_edges << " edges...\n";
        edges = generate_rmat_graph(num_nodes, num_edges);
    } else {
        std::cerr << "Error: Must specify either graph file or synthetic graph parameters\n";
        pgas_finalize(&pgas_ctx);
        return 1;
    }

    std::cout << "Loaded " << edges.size() << " edges\n\n";

    // Build distributed graph
    std::cout << "Building distributed graph...\n";
    PGASGraph<NodeID> graph;
    graph.Init(&pgas_ctx);

    auto build_start = std::chrono::high_resolution_clock::now();
    graph.BuildFromEdgeList(edges, num_nodes, directed);
    auto build_end = std::chrono::high_resolution_clock::now();
    double build_time = std::chrono::duration<double>(build_end - build_start).count();

    graph.PrintStats();
    std::cout << "Build time: " << build_time << " seconds\n\n";

    // Synchronize all nodes before running algorithm
    std::cout << "Synchronizing nodes...\n";
    graph.Barrier();
    std::cout << "All nodes ready.\n\n";

    // Run algorithm
    std::cout << "Running algorithm: " << algorithm << "\n";

    if (algorithm == "bfs") {
        std::cout << "Source vertex: " << source << "\n\n";

        PGASBFS bfs(graph);
        BFSResult result = bfs.Run(source);

        std::cout << "BFS Results:\n";
        std::cout << "  Max depth: " << result.max_depth << "\n";
        std::cout << "  Time: " << result.time_seconds << " seconds\n";

        // Count reachable vertices
        int64_t reachable = 0;
        for (NodeID v = 0; v < num_nodes; v++) {
            if (result.parent[v] != -1) reachable++;
        }
        std::cout << "  Reachable vertices: " << reachable << " / " << num_nodes << "\n";

        if (verify && result.max_depth >= 0) {
            std::cout << "  Verification: PASSED\n";
        }

    } else if (algorithm == "pr") {
        std::cout << "Max iterations: " << pr_max_iters << "\n";
        std::cout << "Epsilon: " << pr_epsilon << "\n\n";

        PGASPageRank pr(graph, 0.85, pr_epsilon, pr_max_iters);
        PageRankResult result = pr.Run();

        std::cout << "PageRank Results:\n";
        std::cout << "  Iterations: " << result.iterations << "\n";
        std::cout << "  Time: " << result.time_seconds << " seconds\n";

        // Find top vertices
        std::vector<std::pair<ScoreT, NodeID>> top_scores;
        for (NodeID v = 0; v < num_nodes; v++) {
            top_scores.emplace_back(result.scores[v], v);
        }
        std::sort(top_scores.rbegin(), top_scores.rend());

        std::cout << "  Top 5 vertices:\n";
        for (int i = 0; i < std::min(5, (int)num_nodes); i++) {
            std::cout << "    " << top_scores[i].second << ": "
                      << top_scores[i].first << "\n";
        }

    } else if (algorithm == "cc") {
        PGASCC cc(graph);
        CCResult result = cc.Run();

        std::cout << "Connected Components Results:\n";
        std::cout << "  Number of components: " << result.num_components << "\n";
        std::cout << "  Time: " << result.time_seconds << " seconds\n";

    } else if (algorithm == "tc") {
        PGASTriangleCounting tc(graph);
        int64_t triangles = tc.Run();

        std::cout << "Triangle Counting Results:\n";
        std::cout << "  Local triangles: " << triangles << "\n";
        // Note: total would need global reduction

    } else {
        std::cerr << "Unknown algorithm: " << algorithm << "\n";
    }

    // Print PGAS statistics
    pgas_stats_t stats;
    pgas_get_stats(&pgas_ctx, &stats);
    std::cout << "\nPGAS Statistics:\n";
    std::cout << "  Local reads: " << stats.local_reads << "\n";
    std::cout << "  Local writes: " << stats.local_writes << "\n";
    std::cout << "  Remote reads: " << stats.remote_reads << "\n";
    std::cout << "  Remote writes: " << stats.remote_writes << "\n";
    std::cout << "  Bytes transferred: " << stats.bytes_transferred << "\n";
    std::cout << "  Avg latency: " << stats.avg_latency_us << " μs\n";

    // Cleanup
    pgas_finalize(&pgas_ctx);

    std::cout << "\nDone.\n";
    return 0;
}
