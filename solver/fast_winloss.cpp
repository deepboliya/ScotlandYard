/**
 * Fast win/loss solver for Scotland Yard.
 *
 * Uses lock-free transposition table, BFS distance pruning, move ordering,
 * and root-level multithreading for maximum performance.
 * Only determines whether Mr. X can guarantee survival (win) or
 * detectives can guarantee capture (loss) — no policy extraction.
 *
 * Build:
 *     g++ -O3 -std=c++17 -pthread -o fast_winloss solver/fast_winloss.cpp
 *
 * Usage:
 *     ./fast_winloss --map maps/full_map.txt --mrx 13 --detectives 7 43 --max-rounds 10
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <queue>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace std;

// ── Graph ──────────────────────────────────────────────────────────────

static constexpr int MAX_NODES = 256;   // 8-bit positions (nodes 1..255)

static vector<int> adj[MAX_NODES];      // adjacency list, 1-indexed
static int num_nodes = 0;               // highest node id seen

static int dist_matrix[MAX_NODES][MAX_NODES];

static void read_map(const string &path) {
    ifstream fin(path);
    if (!fin) { cerr << "Error: cannot open map file: " << path << "\n"; exit(1); }

    set<pair<int,int>> edge_set;
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
    for (int i = 1; i <= num_nodes; ++i) {
        for (int j = 1; j <= num_nodes; ++j)
            dist_matrix[i][j] = 9999;
        dist_matrix[i][i] = 0;
        queue<int> q;
        q.push(i);
        while (!q.empty()) {
            int cur = q.front(); q.pop();
            for (int nb : adj[cur]) {
                if (dist_matrix[i][nb] == 9999) {
                    dist_matrix[i][nb] = dist_matrix[i][cur] + 1;
                    q.push(nb);
                }
            }
        }
    }
}

// ── Lock-free transposition table ──────────────────────────────────────

static constexpr uint64_t TT_SIZE = 1ULL << 24;   // 16M entries ≈ 128 MB
static constexpr uint64_t TT_MASK = TT_SIZE - 1;

static atomic<uint64_t>* TT = nullptr;

// Encoding: use the top 2 bits of the 64-bit entry for the score,
// and the bottom 62 bits for the state key verification.
//   score encoding:  1 → Mr. X wins,  2 → detectives win,  0 → empty
static inline uint64_t pack_tt(uint64_t key, int8_t score) {
    uint64_t enc = (score == 1) ? 1ULL : 2ULL;
    return (key & 0x3FFFFFFFFFFFFFFFULL) | (enc << 62);
}

static inline int8_t tt_probe(uint64_t key, uint64_t idx) {
    uint64_t entry = TT[idx].load(memory_order_relaxed);
    if (entry == 0) return 0;  // empty
    if ((entry & 0x3FFFFFFFFFFFFFFFULL) != (key & 0x3FFFFFFFFFFFFFFFULL))
        return 0;  // collision
    uint64_t enc = entry >> 62;
    return (enc == 1) ? 1 : -1;
}

// ── State encoding ─────────────────────────────────────────────────────
//
// Pack (turn, is_x_turn, x_pos, det0, det1, ...) into a single uint64_t.
// Detective positions are sorted before encoding.
//
// Layout (fits up to 5 detectives with 8-bit node ids):
//   bits [63..57] : turn (7 bits, max 127)
//   bit  [56]     : is_x_turn
//   bits [55..48] : x_pos (8 bits)
//   bits [47..45] : num_dets (3 bits, 0..5)
//   bits [44..37] : det[0]
//   bits [36..29] : det[1]
//   bits [28..21] : det[2]
//   bits [20..13] : det[3]
//   bits [12.. 5] : det[4]
//   bits [ 4.. 0] : unused

static inline uint64_t encode_state(int turn, int x_pos,
                                     const int* dets, int nd,
                                     bool is_x_turn) {
    uint64_t k = (uint64_t)turn;
    k = (k << 1) | (uint64_t)is_x_turn;
    k = (k << 8) | (uint64_t)x_pos;
    k = (k << 3) | (uint64_t)nd;
    for (int i = 0; i < nd; ++i)
        k = (k << 8) | (uint64_t)dets[i];
    // pad remaining slots
    for (int i = nd; i < 5; ++i)
        k <<= 8;
    return k;
}

// ── Early-termination flag ─────────────────────────────────────────────

static atomic<bool> global_win_found{false};

// ── Minimax ────────────────────────────────────────────────────────────

static int MAX_ROUNDS;

// Forward declarations (mutual recursion).
static int8_t fast_minimax(int turn, int x_pos, int* dets, int nd, bool is_x_turn);
static bool   eval_det_combos(int det_idx, int turn, int x_pos,
                               int* dets, int nd);

// Recursive detective move enumeration (one detective at a time).
// Returns true if detectives can guarantee a catch.
static bool eval_det_combos(int det_idx, int turn, int x_pos,
                             int* dets, int nd) {
    if (global_win_found.load(memory_order_relaxed)) return false;

    if (det_idx == nd) {
        // All detectives have moved — sort positions and recurse as Mr. X's turn.
        int sorted[5];
        for (int i = 0; i < nd; ++i) sorted[i] = dets[i];
        sort(sorted, sorted + nd);
        return fast_minimax(turn + 1, x_pos, sorted, nd, true) == -1;
    }

    int orig = dets[det_idx];

    // Move ordering: sort detective moves by distance to Mr. X (closest first).
    const auto &moves = adj[orig];
    int ordered[64];
    int nm = min((int)moves.size(), 64);
    for (int i = 0; i < nm; ++i) ordered[i] = moves[i];
    sort(ordered, ordered + nm, [&](int a, int b) {
        return dist_matrix[a][x_pos] < dist_matrix[b][x_pos];
    });

    for (int i = 0; i < nm; ++i) {
        dets[det_idx] = ordered[i];
        if (eval_det_combos(det_idx + 1, turn, x_pos, dets, nd)) {
            dets[det_idx] = orig;
            return true;
        }
    }

    dets[det_idx] = orig;
    return false;
}

static int8_t fast_minimax(int turn, int x_pos, int* dets, int nd,
                            bool is_x_turn) {
    if (global_win_found.load(memory_order_relaxed)) return 1;

    // Encode and probe TT.
    uint64_t key = encode_state(turn, x_pos, dets, nd, is_x_turn);
    uint64_t idx = (key ^ (key >> 17) ^ (key >> 34)) & TT_MASK;

    int8_t cached = tt_probe(key, idx);
    if (cached != 0) return cached;

    // Terminal: caught?
    for (int i = 0; i < nd; ++i)
        if (x_pos == dets[i]) return -1;

    // Terminal: survived all rounds?
    if (turn >= MAX_ROUNDS) return 1;

    // Distance pruning: if closest detective can't possibly reach X
    // within the remaining turns, X wins immediately.
    int min_dist = 9999;
    for (int i = 0; i < nd; ++i)
        min_dist = min(min_dist, dist_matrix[x_pos][dets[i]]);
    if (min_dist > MAX_ROUNDS - turn) return 1;

    int8_t score;

    if (is_x_turn) {
        // Mr. X — maximiser: try to survive.
        const auto &moves = adj[x_pos];
        if (moves.empty()) return -1;  // trapped

        // Move ordering: prefer moves far from all detectives.
        int ordered[64];
        int nm = min((int)moves.size(), 64);
        for (int i = 0; i < nm; ++i) ordered[i] = moves[i];
        sort(ordered, ordered + nm, [&](int a, int b) {
            int da = 9999, db = 9999;
            for (int j = 0; j < nd; ++j) {
                da = min(da, dist_matrix[a][dets[j]]);
                db = min(db, dist_matrix[b][dets[j]]);
            }
            return da > db;  // farther first
        });

        score = -1;
        for (int i = 0; i < nm; ++i) {
            // After Mr. X moves, it becomes detectives' turn (same round).
            int8_t r = fast_minimax(turn, ordered[i], dets, nd, false);
            if (r == 1) { score = 1; break; }
        }
    } else {
        // Detectives — minimiser: try to catch.
        int dets_copy[5];
        for (int i = 0; i < nd; ++i) dets_copy[i] = dets[i];
        score = eval_det_combos(0, turn, x_pos, dets_copy, nd) ? -1 : 1;
    }

    TT[idx].store(pack_tt(key, score), memory_order_relaxed);
    return score;
}

// ── Root-level parallel search ─────────────────────────────────────────

struct RootResult {
    int move;
    int8_t score;
};

static void root_worker(int x_move, const int* dets, int nd, RootResult &res) {
    int dets_copy[5];
    for (int i = 0; i < nd; ++i) dets_copy[i] = dets[i];

    // After Mr. X moves to x_move, it's detectives' turn at round 1.
    int8_t s = fast_minimax(0, x_move, dets_copy, nd, false);
    res = {x_move, s};
    if (s == 1)
        global_win_found.store(true, memory_order_relaxed);
}

// ── main ───────────────────────────────────────────────────────────────

int main(int argc, char *argv[]) {
    int mrx_start = -1;
    vector<int> det_starts;
    MAX_ROUNDS = 15;
    string map_path;

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--mrx") && i + 1 < argc) {
            mrx_start = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--detectives")) {
            while (i + 1 < argc && strncmp(argv[i + 1], "--", 2) != 0)
                det_starts.push_back(atoi(argv[++i]));
        } else if (!strcmp(argv[i], "--max-rounds") && i + 1 < argc) {
            MAX_ROUNDS = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--map") && i + 1 < argc) {
            map_path = argv[++i];
        }
    }

    if (mrx_start < 0 || det_starts.empty() || map_path.empty()) {
        cerr << "Usage: " << argv[0]
             << " --map <path> --mrx <node>"
                " --detectives <n1> <n2> ... [--max-rounds <N>]\n";
        return 1;
    }

    int nd = (int)det_starts.size();
    if (nd > 5) {
        cerr << "Error: at most 5 detectives supported.\n";
        return 1;
    }

    sort(det_starts.begin(), det_starts.end());

    // ── read map & BFS distances ───────────────────────────────
    read_map(map_path);

    auto node_ok = [](int n) { return n >= 1 && n <= num_nodes && !adj[n].empty(); };
    if (!node_ok(mrx_start)) {
        cerr << "Error: Mr. X node " << mrx_start << " not on board.\n";
        return 1;
    }
    for (int d : det_starts)
        if (!node_ok(d)) {
            cerr << "Error: detective node " << d << " not on board.\n";
            return 1;
        }
    {
        set<int> all(det_starts.begin(), det_starts.end());
        all.insert(mrx_start);
        if ((int)all.size() != nd + 1) {
            cerr << "Error: all starting positions must be distinct.\n";
            return 1;
        }
    }

    cout << "Map: " << map_path << "  (" << num_nodes << " nodes)\n";
    cout << "Mr. X: " << mrx_start << "  |  Detectives:";
    for (int d : det_starts) cout << " " << d;
    cout << "  |  Max rounds: " << MAX_ROUNDS << "\n";

    cout << "Precomputing BFS distances..." << flush;
    precompute_distances();
    cout << " done.\n";

    // ── allocate TT ────────────────────────────────────────────
    TT = new atomic<uint64_t>[TT_SIZE]();
    global_win_found.store(false, memory_order_relaxed);

    // ── solve with root-level parallelism ──────────────────────
    int dets_arr[5];
    for (int i = 0; i < nd; ++i) dets_arr[i] = det_starts[i];

    const auto &first_moves = adj[mrx_start];
    int nm = (int)first_moves.size();

    cout << "Launching " << nm << " root threads (Mr. X moves from "
         << mrx_start << ")...\n" << flush;

    auto t0 = chrono::high_resolution_clock::now();

    vector<thread> workers;
    vector<RootResult> results(nm);

    for (int i = 0; i < nm; ++i)
        workers.emplace_back(root_worker, first_moves[i], dets_arr, nd,
                             ref(results[i]));
    for (auto &t : workers) t.join();

    auto t1 = chrono::high_resolution_clock::now();
    double solve_s = chrono::duration<double>(t1 - t0).count();

    // ── aggregate ──────────────────────────────────────────────
    bool mrx_wins = false;
    int best_move = -1;
    for (auto &r : results) {
        if (r.score == 1) {
            mrx_wins = true;
            best_move = r.move;
            break;
        }
    }

    cout << "\n=== Fast Win/Loss Solve (C++) ===\n";
    cout << "Solve time: " << solve_s << " s\n";
    cout << "Result: " << (mrx_wins ? "MR. X WINS (forced escape)"
                                     : "DETECTIVES WIN (guaranteed catch)") << "\n";
    if (best_move >= 0)
        cout << "Winning first move for Mr. X: " << best_move << "\n";

    delete[] TT;
    return 0;
}
