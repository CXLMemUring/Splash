#ifndef PGAS_ALGORITHMS_H_
#define PGAS_ALGORITHMS_H_

#include "pgas_graph.h"
#include <queue>
#include <limits>
#include <cmath>
#include <omp.h>

// BFS result
struct BFSResult {
    std::vector<NodeID> parent;
    std::vector<int64_t> depth;
    int64_t max_depth;
    double time_seconds;
};

// PageRank result
struct PageRankResult {
    std::vector<ScoreT> scores;
    int iterations;
    double time_seconds;
};

// SSSP result
struct SSSPResult {
    std::vector<int64_t> dist;
    std::vector<NodeID> parent;
    double time_seconds;
};

// Connected Components result
struct CCResult {
    std::vector<NodeID> comp;
    NodeID num_components;
    double time_seconds;
};

// Distributed BFS using direction-optimizing approach
class PGASBFS {
public:
    PGASBFS(PGASGraph<NodeID>& graph) : graph_(graph) {}

    BFSResult Run(NodeID source) {
        BFSResult result;
        auto start = std::chrono::high_resolution_clock::now();

        NodeID num_nodes = graph_.num_nodes();
        result.parent.assign(num_nodes, -1);
        result.depth.assign(num_nodes, -1);

        // Initialize source
        result.parent[source] = source;
        result.depth[source] = 0;

        std::vector<NodeID> frontier;
        std::vector<NodeID> next_frontier;
        frontier.push_back(source);

        int64_t depth = 0;

        while (!frontier.empty()) {
            next_frontier.clear();

            // Process local vertices in frontier
            #pragma omp parallel
            {
                std::vector<NodeID> local_next;

                #pragma omp for nowait
                for (size_t i = 0; i < frontier.size(); i++) {
                    NodeID u = frontier[i];

                    if (graph_.IsLocal(u)) {
                        // Local vertex - direct access
                        for (NodeID v : graph_.OutNeighbors(u)) {
                            if (result.parent[v] == -1) {
                                NodeID expected = -1;
                                if (__sync_bool_compare_and_swap(&result.parent[v], expected, u)) {
                                    result.depth[v] = depth + 1;
                                    local_next.push_back(v);
                                }
                            }
                        }
                    } else {
                        // Remote vertex - fetch neighbors
                        auto neighbors = graph_.GetRemoteNeighbors(u);
                        for (NodeID v : neighbors) {
                            if (result.parent[v] == -1) {
                                NodeID expected = -1;
                                if (__sync_bool_compare_and_swap(&result.parent[v], expected, u)) {
                                    result.depth[v] = depth + 1;
                                    local_next.push_back(v);
                                }
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

            frontier = std::move(next_frontier);
            depth++;

            // Synchronize across nodes
            graph_.Barrier();
        }

        result.max_depth = depth - 1;

        auto end = std::chrono::high_resolution_clock::now();
        result.time_seconds = std::chrono::duration<double>(end - start).count();

        return result;
    }

private:
    PGASGraph<NodeID>& graph_;
};

// Distributed PageRank
class PGASPageRank {
public:
    PGASPageRank(PGASGraph<NodeID>& graph, ScoreT damping = 0.85,
                 ScoreT epsilon = 1e-4, int max_iters = 100)
        : graph_(graph), damping_(damping), epsilon_(epsilon), max_iters_(max_iters) {}

    PageRankResult Run() {
        PageRankResult result;
        auto start = std::chrono::high_resolution_clock::now();

        NodeID num_nodes = graph_.num_nodes();
        NodeID local_start = graph_.LocalStart();
        NodeID local_end = graph_.LocalEnd();
        NodeID local_count = local_end - local_start;

        // Initialize scores
        ScoreT init_score = 1.0 / num_nodes;
        std::vector<ScoreT> scores(num_nodes, init_score);
        std::vector<ScoreT> new_scores(num_nodes, 0);

        // Precompute outgoing contributions
        std::vector<ScoreT> out_contrib(num_nodes);

        int iter;
        for (iter = 0; iter < max_iters_; iter++) {
            // Compute outgoing contributions
            #pragma omp parallel for
            for (NodeID v = local_start; v < local_end; v++) {
                int64_t degree = graph_.OutDegree(v);
                out_contrib[v] = scores[v] / (degree > 0 ? degree : 1);
            }

            // Exchange contributions with other nodes
            graph_.Barrier();

            // Compute new scores
            ScoreT base_score = (1.0 - damping_) / num_nodes;

            #pragma omp parallel for
            for (NodeID v = local_start; v < local_end; v++) {
                ScoreT sum = 0;

                if (graph_.IsLocal(v)) {
                    for (NodeID u : graph_.OutNeighbors(v)) {
                        sum += out_contrib[u];
                    }
                } else {
                    auto neighbors = graph_.GetRemoteNeighbors(v);
                    for (NodeID u : neighbors) {
                        sum += out_contrib[u];
                    }
                }

                new_scores[v] = base_score + damping_ * sum;
            }

            // Check convergence
            ScoreT local_diff = 0;
            #pragma omp parallel for reduction(+:local_diff)
            for (NodeID v = local_start; v < local_end; v++) {
                local_diff += std::abs(new_scores[v] - scores[v]);
            }

            // Swap scores
            std::swap(scores, new_scores);

            graph_.Barrier();

            // Check global convergence (simplified - would need reduction)
            if (local_diff < epsilon_ * local_count) {
                break;
            }
        }

        result.scores = std::move(scores);
        result.iterations = iter;

        auto end = std::chrono::high_resolution_clock::now();
        result.time_seconds = std::chrono::duration<double>(end - start).count();

        return result;
    }

private:
    PGASGraph<NodeID>& graph_;
    ScoreT damping_;
    ScoreT epsilon_;
    int max_iters_;
};

// Distributed SSSP using Bellman-Ford
class PGASSSSP {
public:
    PGASSSSP(PGASGraph<WeightedEdge<>>& graph) : graph_(graph) {}

    SSSPResult Run(NodeID source) {
        SSSPResult result;
        auto start = std::chrono::high_resolution_clock::now();

        NodeID num_nodes = graph_.num_nodes();
        NodeID local_start = graph_.LocalStart();
        NodeID local_end = graph_.LocalEnd();

        const int64_t INF = std::numeric_limits<int64_t>::max() / 2;

        result.dist.assign(num_nodes, INF);
        result.parent.assign(num_nodes, -1);
        result.dist[source] = 0;
        result.parent[source] = source;

        // Bellman-Ford iterations
        for (NodeID i = 0; i < num_nodes - 1; i++) {
            bool changed = false;

            #pragma omp parallel for reduction(|:changed)
            for (NodeID u = local_start; u < local_end; u++) {
                if (result.dist[u] == INF) continue;

                for (auto edge : graph_.OutNeighbors(u)) {
                    NodeID v = edge.dest;
                    int64_t new_dist = result.dist[u] + (int64_t)edge.weight;

                    if (new_dist < result.dist[v]) {
                        result.dist[v] = new_dist;
                        result.parent[v] = u;
                        changed = true;
                    }
                }
            }

            graph_.Barrier();

            if (!changed) break;
        }

        auto end = std::chrono::high_resolution_clock::now();
        result.time_seconds = std::chrono::duration<double>(end - start).count();

        return result;
    }

private:
    PGASGraph<WeightedEdge<>>& graph_;
};

// Distributed Connected Components using Shiloach-Vishkin
class PGASCC {
public:
    PGASCC(PGASGraph<NodeID>& graph) : graph_(graph) {}

    CCResult Run() {
        CCResult result;
        auto start = std::chrono::high_resolution_clock::now();

        NodeID num_nodes = graph_.num_nodes();
        NodeID local_start = graph_.LocalStart();
        NodeID local_end = graph_.LocalEnd();

        // Initialize: each vertex is its own component
        result.comp.resize(num_nodes);
        #pragma omp parallel for
        for (NodeID v = 0; v < num_nodes; v++) {
            result.comp[v] = v;
        }

        bool changed = true;
        while (changed) {
            changed = false;

            // Hooking phase
            #pragma omp parallel for
            for (NodeID u = local_start; u < local_end; u++) {
                for (NodeID v : graph_.OutNeighbors(u)) {
                    NodeID comp_u = result.comp[u];
                    NodeID comp_v = result.comp[v];

                    if (comp_u < comp_v) {
                        NodeID old = __sync_val_compare_and_swap(
                            &result.comp[comp_v], comp_v, comp_u);
                        if (old == comp_v) {
                            changed = true;
                        }
                    }
                }
            }

            graph_.Barrier();

            // Pointer jumping phase
            bool jumped = true;
            while (jumped) {
                jumped = false;

                #pragma omp parallel for
                for (NodeID v = local_start; v < local_end; v++) {
                    NodeID comp_v = result.comp[v];
                    NodeID comp_comp_v = result.comp[comp_v];

                    if (comp_v != comp_comp_v) {
                        result.comp[v] = comp_comp_v;
                        jumped = true;
                    }
                }

                graph_.Barrier();
            }
        }

        // Count components
        std::vector<bool> is_root(num_nodes, false);
        for (NodeID v = 0; v < num_nodes; v++) {
            is_root[result.comp[v]] = true;
        }
        result.num_components = 0;
        for (NodeID v = 0; v < num_nodes; v++) {
            if (is_root[v]) result.num_components++;
        }

        auto end = std::chrono::high_resolution_clock::now();
        result.time_seconds = std::chrono::duration<double>(end - start).count();

        return result;
    }

private:
    PGASGraph<NodeID>& graph_;
};

// Triangle Counting
class PGASTriangleCounting {
public:
    PGASTriangleCounting(PGASGraph<NodeID>& graph) : graph_(graph) {}

    int64_t Run() {
        NodeID local_start = graph_.LocalStart();
        NodeID local_end = graph_.LocalEnd();

        int64_t local_triangles = 0;

        #pragma omp parallel for reduction(+:local_triangles)
        for (NodeID u = local_start; u < local_end; u++) {
            for (NodeID v : graph_.OutNeighbors(u)) {
                if (v > u) {
                    // Count common neighbors
                    auto neighbors_u = graph_.OutNeighbors(u);
                    std::vector<NodeID> neighbors_v_vec;

                    if (graph_.IsLocal(v)) {
                        for (NodeID w : graph_.OutNeighbors(v)) {
                            neighbors_v_vec.push_back(w);
                        }
                    } else {
                        neighbors_v_vec = graph_.GetRemoteNeighbors(v);
                    }

                    // Intersect neighbors
                    auto it_u = neighbors_u.begin();
                    size_t idx_v = 0;

                    while (it_u != neighbors_u.end() && idx_v < neighbors_v_vec.size()) {
                        NodeID nu = *it_u;
                        NodeID nv = neighbors_v_vec[idx_v];

                        if (nu == nv && nu > v) {
                            local_triangles++;
                            ++it_u;
                            ++idx_v;
                        } else if (nu < nv) {
                            ++it_u;
                        } else {
                            ++idx_v;
                        }
                    }
                }
            }
        }

        graph_.Barrier();

        // Would need global reduction here
        return local_triangles;
    }

private:
    PGASGraph<NodeID>& graph_;
};

#endif  // PGAS_ALGORITHMS_H_
