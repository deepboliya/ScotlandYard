/**
 * full_sweep.cpp
 *
 * Exhaustively sweeps the Scotland Yard game tree from all possible Mr. X starting
 * positions against a fixed set of Detective starting positions up to `--max-rounds`.
 *
 * Adapted from fast_policy_v2.2.cpp.
 * Differences:
 *   - Computes survival of Mr. X starting at *every* single node on the board.
 *   - Drops `depth_used` from the memo table, since we do a pure iterative sweep
 *     or always query exactly up to `--max-rounds`. Thus, State is simple:
 *     0 = uncomputed, 1 = computing, 2 = computed (with result in higher bits).
 *   - Prints a live progress bar tracking computed states.
 *
 * Build:
 *   g++ -O3 -std=c++17 -pthread -o full_sweep full_sweep.cpp
 *
 * Usage:
 *   ./full_sweep --map maps/full_map.txt --num-detectives 5 --max-rounds 11 --threads 8
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <functional>
#include <queue>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <memory>
#include <vector>
#include <cstdint>
#include <iomanip>

using namespace std;

static constexpr int MAX_NODES = 256;   // nodes 1..255
static constexpr int INF_DIST = 9999;
static constexpr int MAX_DETS = 5;
static constexpr size_t MAX_TOTAL_STATES = 600000000ULL; // safety cap

static vector<int> adj[MAX_NODES];
static int num_nodes = 0;
static int dist_matrix[MAX_NODES][MAX_NODES];

static int MAX_ROUNDS = 11;
static int NUM_THREADS = 1;
static int ND = 0;

// nCk table up to (num_nodes + MAX_DETS)
static uint64_t nCk[MAX_NODES + MAX_DETS + 1][MAX_DETS + 1];
static size_t num_det_states = 0;
static size_t memo_table_size = 0;

// Packed state for thread-safe concurrent reads/writes:
// Bits 0-7:   State (0 = uncomputed, 1 = computing, 2 = done)
// Bits 8-15:  Value (survival result)
// Because we always evaluate the whole depth iteratively or to completion,
// we don't need 'depth_used' to handle dynamic query depths anymore.
static unique_ptr<atomic<uint32_t>[]> memo_table;

static atomic<uint64_t> states_computed(0);

static void read_map(const string &path) {
    ifstream fin(path);
    if (!fin) {
        cerr << "Error: cannot open map file: " << path << "\n";
        exit(1);
    }

    set<pair<int, int>> edge_set;
    string line;
    while (getline(fin, line)) {
        if (line.empty()) continue;
        istringstream iss(line);
        int u, v;
        if (!(iss >> u >> v)) continue;
        if (u == v) continue;
        if (u > v) swap(u, v);
        edge_set.insert({u, v});
        num_nodes = max(num_nodes, max(u, v));
    }

    if (num_nodes >= MAX_NODES) {
        cerr << "Error: map has node " << num_nodes
             << " but MAX_NODES=" << MAX_NODES << ". Increase MAX_NODES.\n";
        exit(1);
    }

    for (auto &[u, v] : edge_set) {
        adj[u].push_back(v);
        adj[v].push_back(u);
    }
    for (int i = 1; i <= num_nodes; ++i)
        sort(adj[i].begin(), adj[i].end());
}

static void precompute_distances() {
    auto worker = [](atomic<int> &next_src) {
        while (true) {
            int i = next_src.fetch_add(1, memory_order_relaxed);
            if (i > num_nodes) break;

            for (int j = 1; j <= num_nodes; ++j)
                dist_matrix[i][j] = INF_DIST;
            dist_matrix[i][i] = 0;

            queue<int> q;
            q.push(i);
            while (!q.empty()) {
                int cur = q.front();
                q.pop();
                for (int nb : adj[cur]) {
                    if (dist_matrix[i][nb] == INF_DIST) {
                        dist_matrix[i][nb] = dist_matrix[i][cur] + 1;
                        q.push(nb);
                    }
                }
            }
        }
    };

    int tcount = max(1, NUM_THREADS);
    atomic<int> next_src(1);

    if (tcount == 1) {
        worker(next_src);
        return;
    }

    vector<thread> threads;
    threads.reserve(tcount);
    for (int t = 0; t < tcount; ++t)
        threads.emplace_back(worker, ref(next_src));
    for (auto &th : threads) th.join();
}

static void build_nCk() {
    int max_n = num_nodes + MAX_DETS;
    for (int n = 0; n <= max_n; ++n) {
        for (int k = 0; k <= MAX_DETS; ++k) nCk[n][k] = 0;
        nCk[n][0] = 1;
    }
    for (int n = 1; n <= max_n; ++n) {
        for (int k = 1; k <= MAX_DETS; ++k) {
            nCk[n][k] = nCk[n - 1][k] + nCk[n - 1][k - 1];
        }
    }
}

static inline size_t rank_dets(const int* dets, int nd) {
    uint64_t rank = 0;
    for (int i = 0; i < nd; ++i) {
        int y = dets[i] + i;
        rank += nCk[y - 1][i + 1];
    }
    return (size_t)rank;
}

static inline int min_dist_to_dets(int x_pos, const int* dets, int nd) {
    int md = INF_DIST;
    for (int i = 0; i < nd; ++i)
        md = min(md, dist_matrix[x_pos][dets[i]]);
    return md;
}

static inline size_t state_index(int depth_left, int x_pos, const int* dets, int nd, bool is_x_turn) {
    size_t det_rank = rank_dets(dets, nd);
    // Include depth in the key to ensure correctness while sweeping and avoid cyclic dependencies
    // layout: [depth][turn][x_pos][det_rank]
    size_t idx = (size_t)depth_left;
    idx = idx * 2 + (size_t)(is_x_turn ? 1 : 0);
    idx = idx * (size_t)(num_nodes + 1) + (size_t)x_pos;
    idx = idx * num_det_states + det_rank;
    return idx;
}

static int solve(int depth_left, int x_pos, int* dets, int nd, bool is_x_turn);

struct DetRecurseCtx {
    int depth_left;
    int x_pos;
    int nd;
    int orig[5];
    int combo[5];
    int worst_depth;
};

static void det_recurse(DetRecurseCtx &ctx, int det_idx) {
    if (ctx.worst_depth == 0) return;

    if (det_idx == ctx.nd) {
        int sorted[5];
        for (int i = 0; i < ctx.nd; ++i) sorted[i] = ctx.combo[i];
        sort(sorted, sorted + ctx.nd);
        int d = solve(ctx.depth_left, ctx.x_pos, sorted, ctx.nd, true);
        if (d < ctx.worst_depth) {
            ctx.worst_depth = d;
        }
        return;
    }

    const auto &moves = adj[ctx.orig[det_idx]];
    if (moves.empty()) {
        ctx.combo[det_idx] = ctx.orig[det_idx];
        det_recurse(ctx, det_idx + 1);
        return;
    }

    int ordered[64];
    int nm = min((int)moves.size(), 64);
    for (int i = 0; i < nm; ++i) ordered[i] = moves[i];
    sort(ordered, ordered + nm, [&](int a, int b) {
        return dist_matrix[a][ctx.x_pos] < dist_matrix[b][ctx.x_pos];
    });

    for (int i = 0; i < nm; ++i) {
        ctx.combo[det_idx] = ordered[i];
        det_recurse(ctx, det_idx + 1);
        if (ctx.worst_depth == 0) return;
    }
}

static int solve(int depth_left, int x_pos, int* dets, int nd, bool is_x_turn) {
    sort(dets, dets + nd);

    // terminal: caught
    for (int i = 0; i < nd; ++i)
        if (x_pos == dets[i])
            return 0;

    // terminal: horizon reached at X turn
    if (depth_left <= 0 && is_x_turn) return 0;

    size_t idx = state_index(depth_left, x_pos, dets, nd, is_x_turn);
    bool owns_slot = false;

    while (true) {
        uint32_t packed = memo_table[idx].load(memory_order_acquire);
        uint8_t st = packed & 0xFF;
        if (st == 2) {
            return (packed >> 8) & 0xFF;
        }
        if (st == 1) {
            // Another thread computing. Wait/Backoff? 
            // Because depth is strictly encoded in the state_index now, there are NO cyclic dependencies possible!
            // We can actually just yield and wait for the other thread to finish, avoiding all redundant work.
            this_thread::yield(); 
            continue; 
        }
        
        uint32_t expected = 0;
        if (memo_table[idx].compare_exchange_weak(expected, 1, memory_order_acq_rel, memory_order_acquire)) {
            owns_slot = true;
            break; 
        }
    }

    states_computed.fetch_add(1, memory_order_relaxed);

    int result = 0;

    if (is_x_turn) {
        int min_dist = min_dist_to_dets(x_pos, dets, nd);
        if (min_dist > depth_left) {
            result = depth_left;
        } else {
            const auto &moves = adj[x_pos];
            if (moves.empty()) {
                result = 0;
            } else {
                vector<int> ordered = moves;
                sort(ordered.begin(), ordered.end(), [&](int a, int b) {
                    int da = min_dist_to_dets(a, dets, nd);
                    int db = min_dist_to_dets(b, dets, nd);
                    return da > db;
                });

                int best = -1;
                for (int mv : ordered) {
                    int child = solve(depth_left - 1, mv, dets, nd, false);
                    int survival = 1 + child;
                    if (survival > best) best = survival;
                    if (best >= depth_left) break; // Pruned
                }
                
                if (best < 0) best = 0;
                if (best > depth_left) best = depth_left;
                result = best;
            }
        }
    } else {
        // Detectives turn
        DetRecurseCtx ctx;
        ctx.depth_left = depth_left;
        ctx.x_pos = x_pos;
        ctx.nd = nd;
        ctx.worst_depth = depth_left + 1;
        for (int i = 0; i < nd; ++i) ctx.orig[i] = dets[i];

        det_recurse(ctx, 0);
        result = (ctx.worst_depth > depth_left) ? depth_left : ctx.worst_depth;
    }

    if (owns_slot) {
        uint32_t final_pack = 2 | ((uint32_t)result << 8);
        memo_table[idx].store(final_pack, memory_order_release);
    }
    
    return result;
}

static void enumerate_det_tuples_rec(int idx, int lo, int *buf,
                                     const function<void(const int*)> &fn) {
    if (idx == ND) {
        fn(buf);
        return;
    }
    for (int v = lo; v <= num_nodes; ++v) {
        if (adj[v].empty()) continue;
        buf[idx] = v;
        enumerate_det_tuples_rec(idx + 1, v, buf, fn);
    }
}

int main(int argc, char *argv[]) {
    string map_path;

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--num-detectives") && i + 1 < argc) {
            ND = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--max-rounds") && i + 1 < argc) {
            MAX_ROUNDS = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--map") && i + 1 < argc) {
            map_path = argv[++i];
        } else if (!strcmp(argv[i], "--threads") && i + 1 < argc) {
            NUM_THREADS = atoi(argv[++i]);
        } else {
            cerr << "Error: Unknown argument '" << argv[i] << "'\n";
            return 1;
        }
    }

    if (map_path.empty()) {
        cerr << "Usage: " << argv[0]
             << " --map <path>"
             << " [--num-detectives <N>] [--max-rounds <N>] [--threads <T>]\n";
        return 1;
    }

    if (ND <= 0 || ND > MAX_DETS) {
        cerr << "Error: --num-detectives must be between 1 and " << MAX_DETS << ".\n";
        return 1;
    }

    if (NUM_THREADS < 1) {
        cerr << "Error: --threads must be >= 1.\n";
        return 1;
    }

    read_map(map_path);

    cout << "Map: " << map_path << "  (" << num_nodes << " nodes)\n";
    cout << "Detectives: " << ND << "  |  Max rounds: " << MAX_ROUNDS << "  |  Threads: " << NUM_THREADS << "\n";

    cout << "BFS distances..." << flush;
    auto t_bfs0 = chrono::high_resolution_clock::now();
    precompute_distances();
    auto t_bfs1 = chrono::high_resolution_clock::now();
    double bfs_s = chrono::duration<double>(t_bfs1 - t_bfs0).count();
    cout << " done. (" << bfs_s << " s)\n";

    build_nCk();
    num_det_states = (size_t)nCk[num_nodes + ND - 1][ND];

    // Total states calculation now MULTIPLIES by (MAX_ROUNDS + 1)
    size_t states_per_depth = 2ull * (size_t)(num_nodes + 1) * num_det_states;
    size_t total_states = (size_t)(MAX_ROUNDS + 1) * states_per_depth;
    memo_table_size = total_states;

    if (total_states == 0 || total_states > MAX_TOTAL_STATES) {
        cerr << "Error: dense state table too large (" << total_states
             << "). Reduce detectives/rounds.\n";
        return 1;
    }

    memo_table = make_unique<atomic<uint32_t>[]>(total_states);
    for (size_t i = 0; i < total_states; ++i)
        memo_table[i].store(0, memory_order_relaxed);

    cout << "Dense memo states: " << total_states << "\n";
    cout << "Approx memo memory: "
         << (double)(total_states * sizeof(uint32_t)) / (1024.0 * 1024.0) << " MiB\n";

    states_computed.store(0, memory_order_relaxed);

    auto t0 = chrono::high_resolution_clock::now();

    // Start progress bar thread
    atomic<bool> sweep_done(false);
    thread progress_thread([&]() {
        auto start_time = chrono::high_resolution_clock::now();
        while (!sweep_done.load(memory_order_relaxed)) {
            this_thread::sleep_for(chrono::milliseconds(500));
            uint64_t cur = states_computed.load(memory_order_relaxed);
            auto now = chrono::high_resolution_clock::now();
            double elapsed = chrono::duration<double>(now - start_time).count();
            double rate = elapsed > 0 ? cur / elapsed : 0;
            
            double percent = (double)cur / total_states * 100.0;
            if (percent > 100.0) percent = 100.0;
            
            cout << "\r[Progress] Computed: " << cur << " / " << total_states << " states "
                 << "(" << fixed << setprecision(2) << percent << "%) "
                 << "@ " << fixed << setprecision(1) << rate << " states/sec" << flush;
        }
    });

    // Valid Mr. X starting positions
    vector<int> mrx_candidates;
    for (int i = 1; i <= num_nodes; ++i) {
        if (!adj[i].empty()) mrx_candidates.push_back(i);
    }

    // Since we are sweeping ALL detective starting positions and ALL Mr. X starting positions,
    // we need to generate all possible valid starting states.
    vector<pair<int, vector<int>>> starting_states;
    int dets[5];
    enumerate_det_tuples_rec(0, 1, dets, [&](const int* tuple) {
        vector<int> current_dets(tuple, tuple + ND);
        for (int mrx : mrx_candidates) {
            bool is_det = false;
            for (int d : current_dets) if (d == mrx) is_det = true;
            if (!is_det) {
                starting_states.push_back({mrx, current_dets});
            }
        }
    });

    cout << "Total exact starting game states to evaluate: " << starting_states.size() << "\n";

    if (NUM_THREADS > 1) {
        atomic<size_t> next_idx(0);
        vector<thread> workers;
        for (int t = 0; t < NUM_THREADS; ++t) {
            workers.emplace_back([&]() {
                int local_dets[5];
                while (true) {
                    size_t i = next_idx.fetch_add(1, memory_order_relaxed);
                    if (i >= starting_states.size()) break;
                    
                    int mrx = starting_states[i].first;
                    for (int j = 0; j < ND; ++j) local_dets[j] = starting_states[i].second[j];
                    
                    // We don't need to store the result locally because solve() already 
                    // writes it to the global memo_table natively.
                    solve(MAX_ROUNDS, mrx, local_dets, ND, true);
                }
            });
        }
        for (auto &th : workers) th.join();
    } else {
        int local_dets[5];
        for (const auto& state : starting_states) {
            int mrx = state.first;
            for (int j = 0; j < ND; ++j) local_dets[j] = state.second[j];
            solve(MAX_ROUNDS, mrx, local_dets, ND, true);
        }
    }

    sweep_done.store(true, memory_order_relaxed);
    progress_thread.join();

    auto t1 = chrono::high_resolution_clock::now();
    double solve_s = chrono::duration<double>(t1 - t0).count();
    
    cout << "\n\n=== Global Sweep Complete ===\n";
    cout << "Solve time: " << solve_s << " s\n";
    cout << "Total exact states computed: " << states_computed.load(memory_order_relaxed) << "\n\n";

    // Gather statistics
    vector<int> survival_counts(MAX_ROUNDS + 1, 0);
    
    // We already know all starting states evaluated are saved in the top level of the memo_table
    for (const auto& state : starting_states) {
        int mrx = state.first;
        int current_dets[5];
        for(int j=0; j<ND; ++j) current_dets[j] = state.second[j];

        size_t idx = state_index(MAX_ROUNDS, mrx, current_dets, ND, true);
        uint32_t pk = memo_table[idx].load(memory_order_acquire);
        
        int survived_rounds = 0;
        if ((pk & 0xFF) == 2) {
            survived_rounds = (pk >> 8) & 0xFF;
        }
        
        if (survived_rounds >= 0 && survived_rounds <= MAX_ROUNDS) {
            survival_counts[survived_rounds]++;
        }
    }

    cout << "=== Survival Statistics (from " << starting_states.size() << " valid start positions) ===\n";
    int escaped = 0;
    for (int r = 0; r <= MAX_ROUNDS; ++r) {
        cout << "Caught in " << r << " rounds: " << survival_counts[r] << " states\n";
        if (r == MAX_ROUNDS) {
            escaped = survival_counts[r];
        }
    }
    
    cout << "\nSummary: Mr. X escapes (survives full " << MAX_ROUNDS << " rounds) in " 
         << escaped << " out of " << starting_states.size() << " starting positions ("
         << fixed << setprecision(2) << (escaped * 100.0 / starting_states.size()) << "%).\n";

    return 0;
}
