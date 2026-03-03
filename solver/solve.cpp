/**
 * Standalone exhaustive adversarial solver for Scotland Yard.
 *
 * Exact C++ reimplementation of solver/exhaustive_solver.py.
 * Reads a map file, runs minimax with memoisation, and writes
 * a JSON policy file compatible with main.py --policy-file.
 *
 * Build:
 *     g++ -O2 -std=c++17 -o solve solver/solve.cpp
 *
 * Usage:
 *     ./solve --map maps/first50.txt --mrx 10 --detectives 20 30 --max-rounds 5
 *
 * Output:
 *     x10_d20_30_r5_cpp.json   (same format as Python --dump-policy)
 */

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
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

// Enumerate all valid joint detective placements.
// Mirrors _detective_next_states() in Python exactly.
struct DetChild { vector<int> combo; SolverState next; };

static vector<DetChild> detective_next_states(const SolverState &state) {
    const int nd = (int)state.det_positions.size();

    // Per-detective moves (no exclusions — detectives may share nodes).
    vector<vector<int>> per_det(nd);
    for (int i = 0; i < nd; ++i) {
        per_det[i] = valid_moves(state.det_positions[i]);
        if (per_det[i].empty())
            per_det[i] = {state.det_positions[i]};  // stuck
    }

    // Cartesian product with dedup filtering (detectives may share nodes).
    set<vector<int>> seen;
    vector<DetChild> results;
    vector<int> combo(nd);

    function<void(int)> enumerate = [&](int idx) {
        if (idx == nd) {
            {
                vector<int> tmp(combo);
                sort(tmp.begin(), tmp.end());
                if (!seen.insert(tmp).second) return;  // dedup
                // build child
                SolverState nxt;
                nxt.round_number  = state.round_number;
                nxt.is_mrx_turn   = true;
                nxt.mrx_position  = state.mrx_position;
                nxt.det_positions = tmp;          // sorted
                results.push_back({combo, nxt});
            }
            return;
        }
        for (int m : per_det[idx]) {
            combo[idx] = m;
            enumerate(idx + 1);
        }
    };
    enumerate(0);
    return results;
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

    if (state.is_mrx_turn) {
        // Mr. X turn — maximise
        vector<int> moves = valid_moves(state.mrx_position);

        if (moves.empty()) {                       // trapped
            memo[state] = state.round_number;
            return state.round_number;
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
        // Detective turn — minimise
        auto children = detective_next_states(state);

        if (children.empty()) {
            memo[state] = MAX_ROUNDS;
            return MAX_ROUNDS;
        }

        int worst_depth = MAX_ROUNDS + 1;
        vector<int> worst_combo = children[0].combo;

        for (auto &[combo, nxt] : children) {
            int d = get_survival_depth(nxt);
            if (d < worst_depth) { worst_depth = d; worst_combo = combo; }
            if (worst_depth == state.round_number) break;
        }

        det_policy[state] = worst_combo;
        memo[state]       = worst_depth;
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
    string out_path = "x" + to_string(mrx_start) + "_d" + det_str
                    + "_r" + to_string(MAX_ROUNDS) + "_cpp.json";

    write_json(out_path, map_path, mrx_start, det_starts,
               forced_escape, guaranteed, solve_s, memo.size());

    return 0;
}
