/**
 * Optimised policy solver for Scotland Yard  (v2).
 *
 * Key changes over fast_policy.cpp:
 *   1.  State hash depends ONLY on (x_pos, det0..det4, is_x_turn).
 *       Round number is NOT part of the key.  The value stored is "how
 *       many more rounds Mr. X can survive from this position under
 *       optimal play from both sides" (capped at the search depth that
 *       remains when the state is first reached).  Because the minimax
 *       value of a position is round-invariant (both sides optimise the
 *       same quantity regardless of elapsed rounds), a single memo
 *       entry per position suffices.
 *   2.  Positions use uint8_t (nodes 1..200 fit in one byte).
 *   3.  The state key is packed into a uint64_t using only the bits
 *       actually needed (8 bits per position + 1 bit for turn), giving
 *       a smaller key, lower hash-map overhead, and better cache use.
 *   4.  Terminal values:
 *         • caught   → 0  (can survive 0 more rounds)
 *         • survived (depth limit reached) → 0 more rounds left to
 *           survive; the parent adds 1 per X-move on the way up,
 *           so the root gets MAX_ROUNDS when X survives everything.
 *   5.  Distance-pruning and move-ordering are preserved.
 *   6.  JSON output is in the same format as fast_policy.cpp.  String
 *       keys include the round number (for the Python front-end) and
 *       survival_depths are converted back to absolute rounds.
 *
 * Build:
 *     g++ -O3 -std=c++17 -o fast_policy_v2 solver/fast_policy_v2.cpp
 *
 * Usage:
 *     ./fast_policy_v2 --map maps/full_map.txt --mrx 13 --detectives 7 43 --max-rounds 10
 */

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
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

static constexpr int MAX_NODES = 256;   // nodes 1..255 (8-bit)

static vector<int> adj[MAX_NODES];
static int num_nodes = 0;

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
// The memo key depends ONLY on:
//     (is_x_turn, x_pos, det0, det1, ..., det4)
//
// Detective positions are always kept sorted so that permutations map
// to the same key.  Positions fit in 8 bits each (1..200).
//
// Layout inside a uint64_t  (at most 1+8+3+5×8 = 52 bits):
//   bit  [51]     : is_x_turn   (1 bit)
//   bits [50..43] : x_pos       (8 bits)
//   bits [42..40] : num_dets    (3 bits, 0..5)
//   bits [39..32] : det[0]      (8 bits)
//   bits [31..24] : det[1]
//   bits [23..16] : det[2]
//   bits [15.. 8] : det[3]
//   bits [ 7.. 0] : det[4]

static inline uint64_t encode_state(int x_pos, const int* dets, int nd,
                                     bool is_x_turn) {
    uint64_t k = (uint64_t)is_x_turn;
    k = (k << 8) | (uint64_t)(uint8_t)x_pos;
    k = (k << 3) | (uint64_t)(uint8_t)nd;
    for (int i = 0; i < nd; ++i)
        k = (k << 8) | (uint64_t)(uint8_t)dets[i];
    for (int i = nd; i < 5; ++i)
        k <<= 8;
    return k;
}

static inline void decode_state(uint64_t key, bool &is_x_turn,
                                 int &x_pos, int *dets, int &nd) {
    for (int i = 4; i >= 0; --i) {
        dets[i] = (int)(key & 0xFF);
        key >>= 8;
    }
    nd        = (int)(key & 0x7);  key >>= 3;
    x_pos     = (int)(key & 0xFF); key >>= 8;
    is_x_turn = (bool)(key & 1);
}

// String key for JSON output (round-free).
static string state_to_key(bool is_x_turn, int x_pos,
                            const int* dets, int nd) {
    string k = "p=";
    k += (is_x_turn ? "mrx" : "detectives");
    k += "|x=" + to_string(x_pos) + "|d=";
    for (int i = 0; i < nd; ++i) {
        if (i) k += ',';
        k += to_string(dets[i]);
    }
    return k;
}

// ── Solver tables ──────────────────────────────────────────────────────

static int MAX_ROUNDS;

// Memoisation: position key → (survival_value, depth_left_used).
// If the position is later reached with a larger depth_left, we must
// re-solve because the stored value may have been limited by the
// smaller depth_left.
static unordered_map<uint64_t, pair<int,int>> memo;   // key → (value, depth_used)

// Policy: position key → optimal action.
static unordered_map<uint64_t, int>         mrx_policy;
static unordered_map<uint64_t, vector<int>> det_policy;

// ── Minimax with policy extraction ─────────────────────────────────────
//
// depth_left = how many more Mr. X moves remain before the game ends.
// Returns: how many of those depth_left rounds Mr. X can survive under
//          optimal play from both sides.
//
// Why round-free memoisation works
// ─────────────────────────────────
// The minimax value of "how many more moves can X survive" from a
// position is a property of (position, depth_left).  However, depth_left
// only DECREASES along any path (it drops by 1 each time X moves, and
// stays the same on detective turns).  The very first time we visit any
// position it is via the path with the LARGEST possible depth_left
// (since the root starts at MAX_ROUNDS and every branch only decreases).
// A later visit with a SMALLER depth_left can reuse the stored value
// capped to min(stored, depth_left), which is correct because "X can
// survive K rounds from here" implies "X can survive min(K, L) rounds
// when only L rounds remain."

static int solve(int depth_left, int x_pos, int* dets, int nd,
                  bool is_x_turn);

// ── Recursive detective move enumeration ───────────────────────────────

struct DetRecurseCtx {
    int depth_left;
    int x_pos;
    int nd;
    int combo[5];
    int worst_depth;      // best (minimum) survival found (minimiser)
    int worst_combo[5];
    bool found;
};

static void det_recurse(DetRecurseCtx &ctx, int det_idx,
                         const int* orig_dets) {
    if (ctx.worst_depth == 0) return;  // can't do better than immediate catch

    if (det_idx == ctx.nd) {
        int sorted[5];
        for (int i = 0; i < ctx.nd; ++i) sorted[i] = ctx.combo[i];
        sort(sorted, sorted + ctx.nd);

        // Same depth_left, X's turn next.
        int d = solve(ctx.depth_left, ctx.x_pos, sorted, ctx.nd, true);
        if (d < ctx.worst_depth) {
            ctx.worst_depth = d;
            for (int i = 0; i < ctx.nd; ++i) ctx.worst_combo[i] = ctx.combo[i];
            ctx.found = true;
        }
        return;
    }

    const auto &moves = adj[orig_dets[det_idx]];

    if (moves.empty()) {
        ctx.combo[det_idx] = orig_dets[det_idx];
        det_recurse(ctx, det_idx + 1, orig_dets);
        return;
    }

    // Move ordering: closest to X first.
    int ordered[64];
    int nm = min((int)moves.size(), 64);
    for (int i = 0; i < nm; ++i) ordered[i] = moves[i];
    sort(ordered, ordered + nm, [&](int a, int b) {
        return dist_matrix[a][ctx.x_pos] < dist_matrix[b][ctx.x_pos];
    });

    for (int i = 0; i < nm; ++i) {
        ctx.combo[det_idx] = ordered[i];
        det_recurse(ctx, det_idx + 1, orig_dets);
        if (ctx.worst_depth == 0) return;
    }
}

static int solve(int depth_left, int x_pos, int* dets, int nd,
                  bool is_x_turn) {
    // ── terminal: Mr. X caught ──────────────────────────────────
    for (int i = 0; i < nd; ++i)
        if (x_pos == dets[i])
            return 0;   // survives 0 more rounds

    // ── terminal: depth limit reached → X survived ──────────────
    if (depth_left <= 0 && is_x_turn)
        return 0;       // 0 more rounds to play — X has won

    uint64_t key = encode_state(x_pos, dets, nd, is_x_turn);

    auto it = memo.find(key);
    if (it != memo.end()) {
        auto [val, depth_used] = it->second;
        // If stored value didn't hit the depth ceiling, it's exact.
        // If it did hit the ceiling (val == depth_used), we may need
        // to re-solve with more depth.
        if (val < depth_used || depth_left <= depth_used)
            return min(val, depth_left);
        // Otherwise: val == depth_used AND depth_left > depth_used,
        // meaning the search was cut off — fall through to re-solve.
    }

    // ── distance pruning ────────────────────────────────────────
    if (is_x_turn && nd > 0) {
        int min_dist = 9999;
        for (int i = 0; i < nd; ++i)
            min_dist = min(min_dist, dist_matrix[x_pos][dets[i]]);
        if (min_dist > depth_left) {
            memo[key] = {depth_left, depth_left};
            mrx_policy[key] = adj[x_pos].empty() ? x_pos : adj[x_pos][0];
            return depth_left;
        }
    }

    int result;

    if (is_x_turn) {
        // ── Mr. X — maximiser ───────────────────────────────────
        const auto &moves = adj[x_pos];
        if (moves.empty()) {
            memo[key] = {0, depth_left};   // trapped — survives 0 more rounds
            return 0;
        }

        // Move ordering: prefer moves far from detectives.
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
                return da > db;
            });
        }

        int best_depth = -1, best_move = ordered[0];
        bool has_dets = nd > 0;

        for (int i = 0; i < nm; ++i) {
            // X moves → consumes 1 round.
            // Next turn: detectives (or X again if nd==0).
            int d = solve(depth_left - 1, ordered[i], dets, nd,
                          !has_dets);
            // From this state X survives this move (1) + child survival.
            int survival = 1 + d;
            if (survival > best_depth) {
                best_depth = survival;
                best_move = ordered[i];
            }
            if (best_depth >= depth_left) break;
        }

        if (best_depth > depth_left) best_depth = depth_left;

        mrx_policy[key] = best_move;
        memo[key]       = {best_depth, depth_left};
        result = best_depth;

    } else {
        // ── Detectives — minimiser ──────────────────────────────
        DetRecurseCtx ctx;
        ctx.depth_left  = depth_left;
        ctx.x_pos       = x_pos;
        ctx.nd          = nd;
        ctx.worst_depth = depth_left + 1;   // sentinel
        ctx.found       = false;

        det_recurse(ctx, 0, dets);

        result = (ctx.worst_depth > depth_left) ? depth_left : ctx.worst_depth;
        if (ctx.found)
            det_policy[key] = vector<int>(ctx.worst_combo, ctx.worst_combo + nd);
        memo[key] = {result, depth_left};
    }

    return result;
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

// ── Build JSON maps directly from policy tables ────────────────────────

static map<string, int>         sorted_mrx;
static map<string, vector<int>> sorted_det;

static void build_json_maps() {
    for (auto& [key, move] : mrx_policy) {
        bool is_x_turn; int x_pos, nd; int dets[5];
        decode_state(key, is_x_turn, x_pos, dets, nd);
        string skey = state_to_key(is_x_turn, x_pos, dets, nd);
        sorted_mrx[skey] = move;
    }
    for (auto& [key, moves] : det_policy) {
        bool is_x_turn; int x_pos, nd; int dets[5];
        decode_state(key, is_x_turn, x_pos, dets, nd);
        string skey = state_to_key(is_x_turn, x_pos, dets, nd);
        sorted_det[skey] = moves;
    }
}

static void write_json(const string &path,
                        const string &map_path,
                        int mrx_start,
                        const vector<int> &det_starts,
                        int guaranteed_survival,
                        bool forced_escape,
                        double solve_time_s,
                        size_t states_evaluated)
{
    ofstream f(path);
    if (!f) { cerr << "Error: cannot write " << path << "\n"; exit(1); }

    f << "{\n";
    f << "  \"board\": \"" << json_escape(map_path) << "\",\n";

    f << "  \"config\": {\n";
    f << "    \"detective_starts\": [";
    for (size_t i = 0; i < det_starts.size(); ++i)
        f << (i ? ", " : "") << det_starts[i];
    f << "],\n";
    f << "    \"guaranteed_survival\": " << guaranteed_survival << ",\n";
    f << "    \"max_rounds\": " << MAX_ROUNDS << ",\n";
    f << "    \"mrx_start\": " << mrx_start << "\n";
    f << "  },\n";

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

    f << "  \"format\": \"scotlandyard-policy-v3\",\n";

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

    f << "  \"solver\": {\n";
    f << "    \"detective_policy_size\": " << det_policy.size() << ",\n";
    f << "    \"forced_escape\": " << (forced_escape ? "true" : "false") << ",\n";
    f << "    \"memo_size_positions\": " << memo.size() << ",\n";
    f << "    \"policy_size\": " << mrx_policy.size() << ",\n";
    f << "    \"solve_time_seconds\": " << solve_time_s << ",\n";
    f << "    \"states_evaluated\": " << states_evaluated << "\n";
    f << "  }\n";

    f << "}\n";
    f.close();
    cout << "Policy written to: " << path << "\n";
}

// ── Binary output ──────────────────────────────────────────────────────
//
// Compact binary policy format  "SYP1":
//
//   [4 B]   magic           "SYP1"
//   [4 B]   header_len      (uint32 LE)  — length of JSON header blob
//   [hdr B] header           UTF-8 JSON  (config, solver, nd, …)
//   [4 B]   num_mrx          (uint32 LE)
//   [num_mrx × (nd+2) B]     sorted mrx records
//                             each: [x, d0, d1, …, move]   (all uint8)
//   [4 B]   num_det          (uint32 LE)
//   [num_det × (2nd+1) B]    sorted det records
//                             each: [x, d0, d1, …, m0, m1, …] (all uint8)
//
// Records are lexicographically sorted by (x, d0, d1, …) so binary
// search is possible, but in practice we load into a hash-map.

static void write_u32_le(ofstream &f, uint32_t v) {
    char buf[4];
    buf[0] = (char)(v & 0xFF);
    buf[1] = (char)((v >> 8) & 0xFF);
    buf[2] = (char)((v >> 16) & 0xFF);
    buf[3] = (char)((v >> 24) & 0xFF);
    f.write(buf, 4);
}

static void write_binary(const string &path,
                          const string &map_path,
                          int mrx_start,
                          const vector<int> &det_starts,
                          int guaranteed_survival,
                          bool forced_escape,
                          double solve_time_s,
                          size_t states_evaluated)
{
    int nd = (int)det_starts.size();
    int mrx_rec_len = nd + 2;   // x + nd dets + move
    int det_rec_len = 2 * nd + 1; // x + nd dets + nd moves

    // ── build sorted record vectors ────────────────────────────
    struct MrxRec {
        uint8_t data[7]; // max 5 dets + x + move
        bool operator<(const MrxRec &o) const {
            return memcmp(data, o.data, sizeof(data)) < 0;
        }
    };
    struct DetRec {
        uint8_t data[11]; // max x + 5 dets + 5 moves
        bool operator<(const DetRec &o) const {
            return memcmp(data, o.data, sizeof(data)) < 0;
        }
    };

    vector<MrxRec> mrx_recs;
    mrx_recs.reserve(mrx_policy.size());
    for (auto &[key, move] : mrx_policy) {
        bool is_x; int xp, nd2; int dets[5];
        decode_state(key, is_x, xp, dets, nd2);
        MrxRec r{};
        r.data[0] = (uint8_t)xp;
        for (int i = 0; i < nd2; ++i) r.data[1 + i] = (uint8_t)dets[i];
        r.data[1 + nd2] = (uint8_t)move;
        mrx_recs.push_back(r);
    }
    sort(mrx_recs.begin(), mrx_recs.end());

    vector<DetRec> det_recs;
    det_recs.reserve(det_policy.size());
    for (auto &[key, moves] : det_policy) {
        bool is_x; int xp, nd2; int dets[5];
        decode_state(key, is_x, xp, dets, nd2);
        DetRec r{};
        r.data[0] = (uint8_t)xp;
        for (int i = 0; i < nd2; ++i) r.data[1 + i] = (uint8_t)dets[i];
        for (int i = 0; i < (int)moves.size(); ++i)
            r.data[1 + nd2 + i] = (uint8_t)moves[i];
        det_recs.push_back(r);
    }
    sort(det_recs.begin(), det_recs.end());

    // ── build JSON header ──────────────────────────────────────
    ostringstream hdr;
    hdr << "{\n";
    hdr << "  \"format\": \"scotlandyard-policy-bin-v1\",\n";
    hdr << "  \"board\": \"" << json_escape(map_path) << "\",\n";
    hdr << "  \"config\": {\n";
    hdr << "    \"mrx_start\": " << mrx_start << ",\n";
    hdr << "    \"detective_starts\": [";
    for (size_t i = 0; i < det_starts.size(); ++i)
        hdr << (i ? ", " : "") << det_starts[i];
    hdr << "],\n";
    hdr << "    \"max_rounds\": " << MAX_ROUNDS << ",\n";
    hdr << "    \"guaranteed_survival\": " << guaranteed_survival << ",\n";
    hdr << "    \"num_detectives\": " << nd << "\n";
    hdr << "  },\n";
    hdr << "  \"solver\": {\n";
    hdr << "    \"policy_size\": " << mrx_policy.size() << ",\n";
    hdr << "    \"detective_policy_size\": " << det_policy.size() << ",\n";
    hdr << "    \"memo_size_positions\": " << memo.size() << ",\n";
    hdr << "    \"states_evaluated\": " << states_evaluated << ",\n";
    hdr << "    \"solve_time_seconds\": " << solve_time_s << ",\n";
    hdr << "    \"forced_escape\": " << (forced_escape ? "true" : "false") << "\n";
    hdr << "  },\n";
    hdr << "  \"binary_layout\": {\n";
    hdr << "    \"mrx_record_bytes\": " << mrx_rec_len << ",\n";
    hdr << "    \"det_record_bytes\": " << det_rec_len << ",\n";
    hdr << "    \"mrx_record_format\": \"[x, d0..d" << nd - 1 << ", move]\",\n";
    hdr << "    \"det_record_format\": \"[x, d0..d" << nd - 1
        << ", m0..m" << nd - 1 << "]\"\n";
    hdr << "  }\n";
    hdr << "}";
    string header_str = hdr.str();

    // ── write file ─────────────────────────────────────────────
    ofstream f(path, ios::binary);
    if (!f) { cerr << "Error: cannot write " << path << "\n"; exit(1); }

    f.write("SYP1", 4);
    write_u32_le(f, (uint32_t)header_str.size());
    f.write(header_str.data(), header_str.size());

    write_u32_le(f, (uint32_t)mrx_recs.size());
    for (auto &r : mrx_recs)
        f.write((const char*)r.data, mrx_rec_len);

    write_u32_le(f, (uint32_t)det_recs.size());
    for (auto &r : det_recs)
        f.write((const char*)r.data, det_rec_len);

    f.close();

    size_t file_size = 4 + 4 + header_str.size()
                     + 4 + mrx_recs.size() * mrx_rec_len
                     + 4 + det_recs.size() * det_rec_len;
    cout << "Binary policy written to: " << path
         << "  (" << file_size << " bytes, "
         << (file_size / 1024) << " KB)\n";
}

// ── main ───────────────────────────────────────────────────────────────

int main(int argc, char *argv[]) {
    int mrx_start = -1;
    vector<int> det_starts;
    MAX_ROUNDS = 15;
    string map_path;
    string output_format = "binary";  // "json", "binary", or "both"

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
        } else if (!strcmp(argv[i], "--output-format") && i + 1 < argc) {
            output_format = argv[++i];
            if (output_format != "json" && output_format != "binary" && output_format != "both") {
                cerr << "Error: --output-format must be 'json', 'binary', or 'both'.\n";
                return 1;
            }
        }
    }

    if (mrx_start < 0 || det_starts.empty() || map_path.empty()) {
        cerr << "Usage: " << argv[0]
             << " --map <path> --mrx <node>"
                " --detectives <n1> <n2> ... [--max-rounds <N>]"
                " [--output-format json|binary|both]\n";
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
    int guaranteed = solve(MAX_ROUNDS, mrx_start, dets_arr, nd, true);
    auto t1 = chrono::high_resolution_clock::now();
    double solve_s = chrono::duration<double>(t1 - t0).count();

    bool forced_escape = (guaranteed >= MAX_ROUNDS);

    cout << "\n=== Policy Solve (C++) ===\n";
    cout << "Solve time: " << solve_s << " s\n";
    cout << "Memo entries: " << memo.size() << "\n";
    cout << "Mr. X policy size: " << mrx_policy.size() << "\n";
    cout << "Detective policy size: " << det_policy.size() << "\n";
    cout << "Forced escape: " << (forced_escape ? "YES" : "NO") << "\n";

    uint64_t start_key = encode_state(mrx_start, dets_arr, nd, true);
    auto pit = mrx_policy.find(start_key);
    if (pit != mrx_policy.end())
        cout << "Recommended first move for Mr. X: " << pit->second << "\n";

    // ── build output file name stem ─────────────────────────────
    string det_str;
    for (size_t i = 0; i < det_starts.size(); ++i) {
        if (i) det_str += '_';
        det_str += to_string(det_starts[i]);
    }
    string map_name = map_path;
    {
        auto slash = map_name.rfind('/');
        if (slash != string::npos) map_name = map_name.substr(slash + 1);
        auto dot = map_name.rfind('.');
        if (dot != string::npos) map_name = map_name.substr(0, dot);
    }

    // Create policies/ subdirectory if needed.
    filesystem::create_directories("policies");

    string stem = "policies/" + map_name + "_x" + to_string(mrx_start)
                + "_d" + det_str + "_r" + to_string(MAX_ROUNDS) + "_cpp";

    bool emit_json   = (output_format == "json"  || output_format == "both");
    bool emit_binary = (output_format == "binary" || output_format == "both");

    // ── write JSON (if requested) ──────────────────────────────
    double json_ser_s = 0;
    if (emit_json) {
        auto t_ser0 = chrono::high_resolution_clock::now();

        cout << "Building JSON maps from policy tables..." << flush;
        build_json_maps();
        cout << " done. (" << sorted_mrx.size() << " mrx, "
             << sorted_det.size() << " det entries)\n";

        string json_path = stem + ".json";
        write_json(json_path, map_path, mrx_start, det_starts,
                   guaranteed, forced_escape, solve_s, memo.size());

        auto t_ser1 = chrono::high_resolution_clock::now();
        json_ser_s = chrono::duration<double>(t_ser1 - t_ser0).count();
    }

    // ── write compact binary (if requested) ────────────────────
    double bin_ser_s = 0;
    if (emit_binary) {
        auto t_bin0 = chrono::high_resolution_clock::now();

        string bin_path = stem + ".bin";
        write_binary(bin_path, map_path, mrx_start, det_starts,
                     guaranteed, forced_escape, solve_s, memo.size());

        auto t_bin1 = chrono::high_resolution_clock::now();
        bin_ser_s = chrono::duration<double>(t_bin1 - t_bin0).count();
    }

    cout << "\n=== Serialisation Timing ===\n";
    if (emit_json)
        cout << "JSON (build maps + write): " << json_ser_s << " s\n";
    if (emit_binary)
        cout << "Binary (build + write):    " << bin_ser_s  << " s\n";

    return 0;
}
