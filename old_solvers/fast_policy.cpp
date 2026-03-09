/**
 * Fast policy solver for Scotland Yard.
 *
 * Combines speed optimisations from fast_winloss.cpp (static arrays, BFS
 * distance pruning, move ordering) with full optimal policy extraction and
 * JSON output in the same format as solve.cpp.
 *
 * Build:
 *     g++ -O3 -std=c++17 -o fast_policy solver/fast_policy.cpp
 *
 * Usage:
 *     ./fast_policy --map maps/full_map.txt --mrx 13 --detectives 7 43 --max-rounds 10
 */

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <queue>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
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

// ── State encoding ─────────────────────────────────────────────────────
//
// Pack (round, is_x_turn, x_pos, det0, det1, ...) into a single uint64_t.
// Detective positions MUST be sorted before encoding.
//
// Layout (fits up to 5 detectives with 8-bit node ids):
//   bits [63..52] : round  (12 bits, max 4095)
//   bit  [51]     : is_x_turn
//   bits [50..43] : x_pos  (8 bits)
//   bits [42..40] : num_dets (3 bits, 0..5)
//   bits [39..32] : det[0]
//   bits [31..24] : det[1]
//   bits [23..16] : det[2]
//   bits [15.. 8] : det[3]
//   bits [ 7.. 0] : det[4]

static inline uint64_t encode_state(int round, int x_pos,
                                     const int* dets, int nd,
                                     bool is_x_turn) {
    uint64_t k = (uint64_t)round;
    k = (k << 1) | (uint64_t)is_x_turn;
    k = (k << 8) | (uint64_t)x_pos;
    k = (k << 3) | (uint64_t)nd;
    for (int i = 0; i < nd; ++i)
        k = (k << 8) | (uint64_t)dets[i];
    for (int i = nd; i < 5; ++i)
        k <<= 8;
    return k;
}

// Decode a uint64_t key back to state parameters.
static inline void decode_state(uint64_t key, int &round, bool &is_x_turn,
                                 int &x_pos, int *dets, int &nd) {
    round     = (int)(key >> 52);
    is_x_turn = (key >> 51) & 1;
    x_pos     = (key >> 43) & 0xFF;
    nd        = (key >> 40) & 0x7;
    for (int i = 0; i < nd; ++i)
        dets[i] = (key >> (32 - i * 8)) & 0xFF;
}

// Convert state parameters to the string key format used by solve.cpp / main.py.
// Key format:  r=<round>|p=mrx|x=<pos>|d=<d0>,<d1>,...
static string state_to_key(int round, bool is_x_turn, int x_pos,
                            const int* dets, int nd) {
    string k = "r=" + to_string(round)
             + "|p=" + (is_x_turn ? "mrx" : "detectives")
             + "|x=" + to_string(x_pos) + "|d=";
    for (int i = 0; i < nd; ++i) {
        if (i) k += ',';
        k += to_string(dets[i]);
    }
    return k;
}

// Generate string key from an encoded uint64_t key.
static string key_to_string(uint64_t key) {
    int round, nd, x_pos;
    bool is_x_turn;
    int dets[5];
    decode_state(key, round, is_x_turn, x_pos, dets, nd);
    return state_to_key(round, is_x_turn, x_pos, dets, nd);
}

// ── Solver tables ──────────────────────────────────────────────────────

static int MAX_ROUNDS;

// Memoisation: encoded state → survival depth
static unordered_map<uint64_t, int> memo;

// Policy: encoded state → optimal action
static unordered_map<uint64_t, int>         mrx_policy;   // Mr. X → best move
static unordered_map<uint64_t, vector<int>> det_policy;   // Detectives → best combo

// ── Minimax with policy extraction ─────────────────────────────────────

static int get_survival_depth(int round, int x_pos, int* dets, int nd,
                               bool is_x_turn);

// ── Recursive detective move enumeration ───────────────────────────────

struct DetRecurseCtx {
    int round;
    int x_pos;
    int nd;
    int combo[5];         // current combo being built
    int worst_depth;      // best (minimum) depth found so far
    int worst_combo[5];   // combo that achieved worst_depth
    bool found;
};

static void det_recurse(DetRecurseCtx &ctx, int det_idx,
                         const int* orig_dets) {
    if (ctx.worst_depth == ctx.round) return;  // can't do better (caught this round)

    if (det_idx == ctx.nd) {
        // All detectives placed — sort positions and recurse as Mr. X's turn.
        int sorted[5];
        for (int i = 0; i < ctx.nd; ++i) sorted[i] = ctx.combo[i];
        sort(sorted, sorted + ctx.nd);

        // Same round, Mr. X's turn next.
        int d = get_survival_depth(ctx.round, ctx.x_pos, sorted, ctx.nd, true);
        if (d < ctx.worst_depth) {
            ctx.worst_depth = d;
            for (int i = 0; i < ctx.nd; ++i) ctx.worst_combo[i] = ctx.combo[i];
            ctx.found = true;
        }
        return;
    }

    const auto &moves = adj[orig_dets[det_idx]];

    if (moves.empty()) {
        // Detective is stuck — stays in place.
        ctx.combo[det_idx] = orig_dets[det_idx];
        det_recurse(ctx, det_idx + 1, orig_dets);
        return;
    }

    // Move ordering: sort detective moves by distance to Mr. X (closest first).
    int ordered[64];
    int nm = min((int)moves.size(), 64);
    for (int i = 0; i < nm; ++i) ordered[i] = moves[i];
    sort(ordered, ordered + nm, [&](int a, int b) {
        return dist_matrix[a][ctx.x_pos] < dist_matrix[b][ctx.x_pos];
    });

    for (int i = 0; i < nm; ++i) {
        ctx.combo[det_idx] = ordered[i];
        det_recurse(ctx, det_idx + 1, orig_dets);
        if (ctx.worst_depth == ctx.round) return;  // early exit
    }
}

static int get_survival_depth(int round, int x_pos, int* dets, int nd,
                               bool is_x_turn) {
    uint64_t key = encode_state(round, x_pos, dets, nd, is_x_turn);

    auto it = memo.find(key);
    if (it != memo.end()) return it->second;

    // ── terminal: Mr. X caught ──────────────────────────────────
    for (int i = 0; i < nd; ++i)
        if (x_pos == dets[i]) {
            memo[key] = round;
            return round;
        }

    // ── terminal: survived all rounds ───────────────────────────
    if (round >= MAX_ROUNDS && is_x_turn) {
        memo[key] = MAX_ROUNDS;
        return MAX_ROUNDS;
    }

    // ── distance pruning ────────────────────────────────────────
    if (is_x_turn && nd > 0) {
        int min_dist = 9999;
        for (int i = 0; i < nd; ++i)
            min_dist = min(min_dist, dist_matrix[x_pos][dets[i]]);
        if (min_dist > MAX_ROUNDS - round) {
            memo[key] = MAX_ROUNDS;
            mrx_policy[key] = adj[x_pos].empty() ? x_pos : adj[x_pos][0];
            return MAX_ROUNDS;
        }
    }

    int result;

    if (is_x_turn) {
        // ── Mr. X — maximiser: survive as long as possible ──────
        const auto &moves = adj[x_pos];
        if (moves.empty()) {
            memo[key] = round;  // trapped
            return round;
        }

        // Move ordering: prefer moves far from all detectives.
        int ordered[64];
        int nm = min((int)moves.size(), 64);
        for (int i = 0; i < nm; ++i) ordered[i] = moves[i];
        if (nd > 0) {
            sort(ordered, ordered + nm, [&](int a, int b) {
                int da = 9999, db = 9999;
                for (int j = 0; j < nd; ++j) {
                    da = min(da, dist_matrix[a][dets[j]]);
                    db = min(db, dist_matrix[b][dets[j]]);
                }
                return da > db;  // farther first
            });
        }

        int best_depth = -1, best_move = ordered[0];
        bool has_dets = nd > 0;

        for (int i = 0; i < nm; ++i) {
            // After Mr. X moves, round increments; detectives go next
            // (or Mr. X again if no detectives).
            int d = get_survival_depth(round + 1, ordered[i], dets, nd,
                                        !has_dets);
            if (d > best_depth) {
                best_depth = d;
                best_move = ordered[i];
            }
            if (best_depth == MAX_ROUNDS) break;  // can't do better
        }

        mrx_policy[key] = best_move;
        memo[key]       = best_depth;
        result = best_depth;

    } else {
        // ── Detectives — minimiser: catch Mr. X as early as possible ──
        DetRecurseCtx ctx;
        ctx.round       = round;
        ctx.x_pos       = x_pos;
        ctx.nd          = nd;
        ctx.worst_depth = MAX_ROUNDS + 1;
        ctx.found       = false;

        det_recurse(ctx, 0, dets);

        result = (ctx.worst_depth > MAX_ROUNDS) ? MAX_ROUNDS : ctx.worst_depth;
        if (ctx.found)
            det_policy[key] = vector<int>(ctx.worst_combo, ctx.worst_combo + nd);
        memo[key] = result;
    }

    return result;
}

// ── JSON output (same format as solve.cpp) ─────────────────────────────

static string json_escape(const string &s) {
    string out;
    for (char c : s) {
        if (c == '"')       out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else                out += c;
    }
    return out;
}

static void write_json(const string &path,
                        const string &map_path,
                        int mrx_start,
                        const vector<int> &det_starts,
                        bool forced_escape,
                        int guaranteed_depth,
                        double solve_time_s,
                        size_t states_evaluated)
{
    // Convert uint64_t keys to string keys, sorted for deterministic output
    // (matches Python json sort_keys=True).
    map<string, int>         sorted_mrx;
    map<string, vector<int>> sorted_det;
    map<string, int>         sorted_depths;

    for (auto &[k, m] : mrx_policy)
        sorted_mrx[key_to_string(k)] = m;
    for (auto &[k, m] : det_policy)
        sorted_det[key_to_string(k)] = m;
    for (auto &[k, d] : memo)
        sorted_depths[key_to_string(k)] = d;

    ofstream f(path);
    if (!f) { cerr << "Error: cannot write " << path << "\n"; exit(1); }

    f << "{\n";
    f << "  \"board\": \"" << json_escape(map_path) << "\",\n";

    // config
    f << "  \"config\": {\n";
    f << "    \"detective_starts\": [";
    for (size_t i = 0; i < det_starts.size(); ++i)
        f << (i ? ", " : "") << det_starts[i];
    f << "],\n";
    f << "    \"max_rounds\": " << MAX_ROUNDS << ",\n";
    f << "    \"mrx_start\": " << mrx_start << "\n";
    f << "  },\n";

    // detective_policy
    f << "  \"detective_policy\": {\n";
    {
        bool first = true;
        for (auto &[k, v] : sorted_det) {
            if (!first) f << ",\n";
            f << "    \"" << json_escape(k) << "\": [";
            for (size_t i = 0; i < v.size(); ++i)
                f << (i ? ", " : "") << v[i];
            f << "]";
            first = false;
        }
    }
    f << "\n  },\n";

    // format
    f << "  \"format\": \"scotlandyard-policy-v2\",\n";

    // policy (Mr. X)
    f << "  \"policy\": {\n";
    {
        bool first = true;
        for (auto &[k, v] : sorted_mrx) {
            if (!first) f << ",\n";
            f << "    \"" << json_escape(k) << "\": " << v;
            first = false;
        }
    }
    f << "\n  },\n";

    // survival_depths (all visited states)
    f << "  \"survival_depths\": {\n";
    {
        bool first = true;
        for (auto &[k, v] : sorted_depths) {
            if (!first) f << ",\n";
            f << "    \"" << json_escape(k) << "\": " << v;
            first = false;
        }
    }
    f << "\n  },\n";

    // solver
    f << "  \"solver\": {\n";
    f << "    \"detective_policy_size\": " << det_policy.size() << ",\n";
    f << "    \"forced_escape\": " << (forced_escape ? "true" : "false") << ",\n";
    f << "    \"guaranteed_survival_depth\": " << guaranteed_depth << ",\n";
    f << "    \"guaranteed_survival_rounds\": " << guaranteed_depth << ",\n";
    f << "    \"policy_size\": " << mrx_policy.size() << ",\n";
    f << "    \"solve_time_seconds\": " << solve_time_s << ",\n";
    f << "    \"states_evaluated\": " << states_evaluated << ",\n";
    f << "    \"survival_depths_size\": " << sorted_depths.size() << "\n";
    f << "  }\n";

    f << "}\n";
    f.close();
    cout << "Policy written to: " << path << "\n";
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

    // ── solve ──────────────────────────────────────────────────
    int dets_arr[5];
    for (int i = 0; i < nd; ++i) dets_arr[i] = det_starts[i];

    auto t0 = chrono::high_resolution_clock::now();
    int guaranteed = get_survival_depth(0, mrx_start, dets_arr, nd, true);
    auto t1 = chrono::high_resolution_clock::now();
    double solve_s = chrono::duration<double>(t1 - t0).count();

    bool forced_escape = (guaranteed >= MAX_ROUNDS);

    cout << "\n=== Fast Policy Solve (C++) ===\n";
    cout << "Solve time: " << solve_s << " s\n";
    cout << "States evaluated: " << memo.size() << "\n";
    cout << "Mr. X policy size: " << mrx_policy.size() << "\n";
    cout << "Detective policy size: " << det_policy.size() << "\n";
    cout << "Guaranteed survival depth: " << guaranteed << "\n";
    cout << "Guaranteed survival rounds from start: " << guaranteed << "\n";
    cout << "Forced escape: " << (forced_escape ? "YES" : "NO") << "\n";

    // Find recommended first move.
    uint64_t start_key = encode_state(0, mrx_start, dets_arr, nd, true);
    auto pit = mrx_policy.find(start_key);
    if (pit != mrx_policy.end())
        cout << "Recommended first move for Mr. X: " << pit->second << "\n";

    // ── write JSON ─────────────────────────────────────────────
    string det_str;
    for (size_t i = 0; i < det_starts.size(); ++i) {
        if (i) det_str += '_';
        det_str += to_string(det_starts[i]);
    }
    // Extract map name (strip directory prefix and .txt suffix).
    string map_name = map_path;
    {
        auto slash = map_name.rfind('/');
        if (slash != string::npos) map_name = map_name.substr(slash + 1);
        auto dot = map_name.rfind('.');
        if (dot != string::npos) map_name = map_name.substr(0, dot);
    }
    string out_path = map_name + "_x" + to_string(mrx_start) + "_d" + det_str
                    + "_r" + to_string(MAX_ROUNDS) + "_cpp.json";

    write_json(out_path, map_path, mrx_start, det_starts,
               forced_escape, guaranteed, solve_s, memo.size());

    return 0;
}
