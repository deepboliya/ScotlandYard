/**
 * Standalone exhaustive adversarial solver for Scotland Yard.
 *
 * Exact C++ reimplementation of solver/exhaustive_solver.py.
 * Reads a map file, runs minimax with memoisation, and writes
 * a JSON policy file compatible with main.py --policy-file.
 *
 * Build:
 *     g++ -O3 -std=c++17 -o solve solver/solve.cpp
 *
 * Usage:
 *     ./solve --map maps/first50.txt --mrx 10 --detectives 20 30 --max-rounds 5
 *
 * Output:
 *     first50_x10_d20_30_r5_cpp.json   (same format as Python --dump-policy)
 */

#include <algorithm>
#include <cassert>
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

static vector<vector<int>> adj;   // adjacency list (1-indexed)
static int max_node = 0;

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
        max_node = max(max_node, max(u, v));
    }

    adj.assign(max_node + 1, {});
    for (auto &[u, v] : edge_set) {
        adj[u].push_back(v);
        adj[v].push_back(u);
    }
    for (auto &a : adj) sort(a.begin(), a.end());
}

// ── BFS distance matrix ────────────────────────────────────────────────

static vector<vector<int>> dist_matrix;  // dist_matrix[u][v] = shortest path

static void precompute_distances() {
    dist_matrix.assign(max_node + 1, vector<int>(max_node + 1, 9999));
    for (int i = 1; i <= max_node; ++i) {
        if (adj[i].empty()) continue;
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

// ── State ──────────────────────────────────────────────────────────────

struct SolverState {
    int  round_number;
    bool is_mrx_turn;          // true ↔ "mrx", false ↔ "detectives"
    int  mrx_position;
    vector<int> det_positions;  // always kept sorted

    bool operator==(const SolverState &o) const {
        return round_number  == o.round_number  &&
               is_mrx_turn   == o.is_mrx_turn   &&
               mrx_position  == o.mrx_position  &&
               det_positions  == o.det_positions;
    }
};

struct StateHash {
    size_t operator()(const SolverState &s) const {
        size_t h = hash<int>()(s.round_number);
        h ^= hash<int>()(s.is_mrx_turn) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= hash<int>()(s.mrx_position) + 0x9e3779b9 + (h << 6) + (h >> 2);
        for (int d : s.det_positions)
            h ^= hash<int>()(d) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

// Key format identical to Python _state_to_key()
static string state_to_key(const SolverState &s) {
    string k = "r=" + to_string(s.round_number)
             + "|p=" + (s.is_mrx_turn ? "mrx" : "detectives")
             + "|x=" + to_string(s.mrx_position) + "|d=";
    for (size_t i = 0; i < s.det_positions.size(); ++i) {
        if (i) k += ',';
        k += to_string(s.det_positions[i]);
    }
    return k;
}

// ── Move generation ────────────────────────────────────────────────────

// Neighbours of `node`.
static vector<int> valid_moves(int node) {
    return adj[node];  // already sorted
}

// ── Solver tables ──────────────────────────────────────────────────────

static unordered_map<SolverState, int,         StateHash> memo;
static unordered_map<SolverState, int,         StateHash> mrx_policy;
static unordered_map<SolverState, vector<int>, StateHash> det_policy;
static int MAX_ROUNDS;

// ── Minimax ────────────────────────────────────────────────────────────

static int get_survival_depth(const SolverState &state);

// ── Recursive per-detective move enumeration with early cutoff ──────
//
// Instead of generating the full Cartesian product of detective moves
// upfront, we recurse one detective at a time.  At each leaf (all
// detectives placed) we call get_survival_depth and track the global
// minimum.  We short-circuit as soon as worst_depth equals the current
// round (detectives caught Mr. X — can't do better).

struct DetRecurseCtx {
    int              round_number;
    int              mrx_position;
    int              nd;
    vector<vector<int>> per_det;     // per_det[i] = ordered moves for det i
    vector<int>      combo;          // current combo being built
    int              worst_depth;    // best (minimum) depth found so far
    vector<int>      worst_combo;    // combo that achieved worst_depth
};

static void det_recurse(DetRecurseCtx &ctx, int det_idx) {
    if (ctx.worst_depth == ctx.round_number) return;  // can't do better

    if (det_idx == ctx.nd) {
        // All detectives placed — build child state and evaluate.
        vector<int> sorted_combo(ctx.combo);
        sort(sorted_combo.begin(), sorted_combo.end());

        SolverState nxt;
        nxt.round_number  = ctx.round_number;
        nxt.is_mrx_turn   = true;
        nxt.mrx_position  = ctx.mrx_position;
        nxt.det_positions = sorted_combo;

        int d = get_survival_depth(nxt);
        if (d < ctx.worst_depth) {
            ctx.worst_depth = d;
            ctx.worst_combo = ctx.combo;   // unsorted original
        }
        return;
    }

    for (int m : ctx.per_det[det_idx]) {
        ctx.combo[det_idx] = m;
        det_recurse(ctx, det_idx + 1);
        if (ctx.worst_depth == ctx.round_number) return;  // early exit
    }
}

static int get_survival_depth(const SolverState &state) {
    auto it = memo.find(state);
    if (it != memo.end()) return it->second;

    // ── terminal: Mr. X caught ──────────────────────────────────
    for (int d : state.det_positions)
        if (d == state.mrx_position) {
            memo[state] = state.round_number;
            return state.round_number;
        }

    // ── terminal: survived all rounds ───────────────────────────
    if (state.round_number >= MAX_ROUNDS && state.is_mrx_turn) {
        memo[state] = MAX_ROUNDS;
        return MAX_ROUNDS;
    }

    // ── distance pruning: closest detective can't reach X in time ──
    if (state.is_mrx_turn && !state.det_positions.empty()) {
        int min_dist = 9999;
        for (int d : state.det_positions)
            min_dist = min(min_dist, dist_matrix[state.mrx_position][d]);
        if (min_dist > MAX_ROUNDS - state.round_number) {
            memo[state] = MAX_ROUNDS;
            mrx_policy[state] = valid_moves(state.mrx_position).empty()
                ? state.mrx_position : valid_moves(state.mrx_position)[0];
            return MAX_ROUNDS;
        }
    }

    if (state.is_mrx_turn) {
        // Mr. X turn — maximise
        vector<int> moves = valid_moves(state.mrx_position);

        if (moves.empty()) {                       // trapped
            memo[state] = state.round_number;
            return state.round_number;
        }

        // Move ordering: prefer moves farthest from all detectives.
        if (!state.det_positions.empty()) {
            sort(moves.begin(), moves.end(), [&](int a, int b) {
                int da = 9999, db = 9999;
                for (int d : state.det_positions) {
                    da = min(da, dist_matrix[a][d]);
                    db = min(db, dist_matrix[b][d]);
                }
                return da > db;  // farther first
            });
        }

        int best_depth = -1, best_move = moves[0];
        bool has_dets = !state.det_positions.empty();

        for (int move : moves) {
            SolverState nxt;
            nxt.round_number  = state.round_number + 1;
            nxt.is_mrx_turn   = !has_dets;        // detectives next (or mrx if 0 dets)
            nxt.mrx_position  = move;
            nxt.det_positions = state.det_positions;

            int d = get_survival_depth(nxt);
            if (d > best_depth) { best_depth = d; best_move = move; }
            if (best_depth == MAX_ROUNDS) break;
        }

        mrx_policy[state] = best_move;
        memo[state]       = best_depth;
        return best_depth;

    } else {
        // Detective turn — minimise (recursive per-detective enumeration)
        const int nd = (int)state.det_positions.size();
        const int xpos = state.mrx_position;

        DetRecurseCtx ctx;
        ctx.round_number = state.round_number;
        ctx.mrx_position = xpos;
        ctx.nd           = nd;
        ctx.combo.resize(nd);
        ctx.worst_depth  = MAX_ROUNDS + 1;

        // Per-detective moves, ordered by distance to Mr. X (closest first).
        ctx.per_det.resize(nd);
        for (int i = 0; i < nd; ++i) {
            ctx.per_det[i] = valid_moves(state.det_positions[i]);
            if (ctx.per_det[i].empty())
                ctx.per_det[i] = {state.det_positions[i]};  // stuck
            sort(ctx.per_det[i].begin(), ctx.per_det[i].end(),
                 [&](int a, int b) {
                     return dist_matrix[a][xpos] < dist_matrix[b][xpos];
                 });
        }

        det_recurse(ctx, 0);

        int worst_depth = (ctx.worst_depth > MAX_ROUNDS) ? MAX_ROUNDS : ctx.worst_depth;
        if (!ctx.worst_combo.empty())
            det_policy[state] = ctx.worst_combo;
        memo[state] = worst_depth;
        return worst_depth;
    }
}

// ── JSON output ────────────────────────────────────────────────────────

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
    // Sort policies by key for deterministic output (matches Python json sort_keys=True)
    map<string, int>         sorted_mrx;
    map<string, vector<int>> sorted_det;
    map<string, int>         sorted_survival_depths;
    for (auto &[s, m] : mrx_policy) sorted_mrx[state_to_key(s)] = m;
    for (auto &[s, m] : det_policy) sorted_det[state_to_key(s)] = m;
    for (auto &[s, d] : memo) sorted_survival_depths[state_to_key(s)] = d;

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

    // policy
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
        for (auto &[k, v] : sorted_survival_depths) {
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
    f << "    \"survival_depths_size\": " << sorted_survival_depths.size() << "\n";
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
                " --detectives <n1> <n2> ... --max-rounds <N>\n";
        return 1;
    }

    sort(det_starts.begin(), det_starts.end());

    // ── read map ───────────────────────────────────────────────
    read_map(map_path);
    cout << "Precomputing BFS distances..." << flush;
    precompute_distances();
    cout << " done." << endl;

    // ── validate positions ─────────────────────────────────────
    auto node_ok = [](int n) { return n >= 1 && n <= max_node && !adj[n].empty(); };
    if (!node_ok(mrx_start)) {
        cerr << "Error: Mr. X start node " << mrx_start << " is not on the board.\n";
        return 1;
    }
    for (int d : det_starts)
        if (!node_ok(d)) {
            cerr << "Error: detective start node " << d << " is not on the board.\n";
            return 1;
        }
    {
        set<int> all_pos(det_starts.begin(), det_starts.end());
        all_pos.insert(mrx_start);
        if ((int)all_pos.size() != (int)det_starts.size() + 1) {
            cerr << "Error: all starting positions must be distinct.\n";
            return 1;
        }
    }

    // ── solve ──────────────────────────────────────────────────
    SolverState start;
    start.round_number  = 0;
    start.is_mrx_turn   = true;
    start.mrx_position  = mrx_start;
    start.det_positions  = det_starts;

    auto t0 = chrono::high_resolution_clock::now();
    int guaranteed = get_survival_depth(start);
    auto t1 = chrono::high_resolution_clock::now();
    double solve_s = chrono::duration<double>(t1 - t0).count();

    bool forced_escape = (guaranteed >= MAX_ROUNDS);

    cout << "\n=== Exhaustive Adversarial Solve (C++) ===\n";
    cout << "Solve time: " << solve_s << " s\n";
    cout << "States evaluated: " << memo.size() << "\n";
    cout << "Mr. X policy size: " << mrx_policy.size() << "\n";
    cout << "Guaranteed survival depth: " << guaranteed << "\n";
    cout << "Guaranteed survival rounds from start: " << guaranteed << "\n";
    cout << "Forced escape: " << (forced_escape ? "YES" : "NO") << "\n";

    auto pit = mrx_policy.find(start);
    if (pit != mrx_policy.end())
        cout << "Recommended first move for Mr. X: " << pit->second << "\n";

    // ── write JSON ─────────────────────────────────────────────
    string det_str;
    for (size_t i = 0; i < det_starts.size(); ++i) {
        if (i) det_str += '_';
        det_str += to_string(det_starts[i]);
    }
    // Extract map name (strip directory prefix and .txt suffix)
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
