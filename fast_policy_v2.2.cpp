/**
 * fast_policy_v2.2.cpp
 *
 * Dense-table Scotland Yard solver (generalized, up to 5 detectives).
 *
 * Differences vs fast_policy_v2.1.cpp:
 *   - Fixes a data race by packing memo_state, memo_value, and memo_depth_used
 *     into a single std::atomic<uint32_t> ensuring thread-safe re-evaluations.
 *
 * Build:
 *   g++ -O3 -std=c++17 -pthread -o fast_policy_v2_2 fast_policy_v2.2.cpp
 *
 * Usage:
 *   ./fast_policy_v2_2 --map maps/full_map.txt --mrx 100 --detectives 1 199 --max-rounds 12 --threads 8 --suffix exp1
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

using namespace std;

static constexpr int MAX_NODES = 256;   // nodes 1..255
static constexpr int INF_DIST = 9999;
static constexpr int MAX_DETS = 5;
static constexpr size_t MAX_TOTAL_STATES = 600000000ULL; // safety cap

static vector<int> adj[MAX_NODES];
static int num_nodes = 0;
static int dist_matrix[MAX_NODES][MAX_NODES];

static int MAX_ROUNDS = 15;
static int NUM_THREADS = 1;
static int ND = 0;

// nCk table up to (num_nodes + MAX_DETS)
static uint64_t nCk[MAX_NODES + MAX_DETS + 1][MAX_DETS + 1];
static size_t num_det_states = 0;
static size_t memo_table_size = 0;

// Packed state for thread-safe concurrent reads/writes:
// Bits 0-7:   State (0 = uncomputed, 1 = computing, 2 = done)
// Bits 8-15:  Value (survival result)
// Bits 16-31: Depth Used
static unique_ptr<atomic<uint32_t>[]> memo_table;

static vector<uint8_t> mrx_best_move;
static vector<uint8_t> mrx_policy_set;
static vector<uint8_t> det_policy_flat;
static vector<uint8_t> det_policy_set;
static atomic<uint64_t> states_computed(0);

static map<string, int>         sorted_mrx;
static map<string, vector<int>> sorted_det;
static map<string, int>         sorted_survival;

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

// Rank sorted multiset dets[0..nd-1] with repetition, values in [1..num_nodes].
// Transform to strict-combination via y_i = det_i + i (0-based), then colex rank:
// rank = sum C(y_i - 1, i+1)
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

static inline size_t state_index(int x_pos, const int* dets, int nd, bool is_x_turn) {
    size_t det_rank = rank_dets(dets, nd);
    // layout: [turn][x_pos][det_rank]
    size_t idx = (size_t)(is_x_turn ? 1 : 0);
    idx = idx * (size_t)(num_nodes + 1) + (size_t)x_pos;
    idx = idx * num_det_states + det_rank;
    return idx;
}

static int solve(int depth_left, int x_pos, int* dets, int nd,
                 bool is_x_turn, bool allow_root_parallel);

struct DetRecurseCtx {
    int depth_left;
    int x_pos;
    int nd;
    int orig[5];
    int combo[5];
    int worst_depth;
    int worst_combo[5];
    bool found;
};

static void det_recurse(DetRecurseCtx &ctx, int det_idx) {
    if (ctx.worst_depth == 0) return;

    if (det_idx == ctx.nd) {
        int sorted[5];
        for (int i = 0; i < ctx.nd; ++i) sorted[i] = ctx.combo[i];
        sort(sorted, sorted + ctx.nd);
        int d = solve(ctx.depth_left, ctx.x_pos, sorted, ctx.nd, true, false);
        if (d < ctx.worst_depth) {
            ctx.worst_depth = d;
            for (int i = 0; i < ctx.nd; ++i) ctx.worst_combo[i] = ctx.combo[i];
            ctx.found = true;
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

static int solve(int depth_left, int x_pos, int* dets, int nd,
                 bool is_x_turn, bool allow_root_parallel) {
    sort(dets, dets + nd);

    // terminal: caught
    for (int i = 0; i < nd; ++i)
        if (x_pos == dets[i])
            return 0;

    // terminal: horizon reached at X turn
    if (depth_left <= 0 && is_x_turn) return 0;

    size_t idx = state_index(x_pos, dets, nd, is_x_turn);
    bool owns_slot = false;

    while (true) {
        uint32_t packed = memo_table[idx].load(memory_order_acquire);
        uint8_t st = packed & 0xFF;
        if (st == 2) {
            int val = (packed >> 8) & 0xFF;
            int depth_used = (packed >> 16) & 0xFFFF;

            // - exact if val < depth_used
            // - reusable if query depth <= depth_used
            if (val < depth_used || depth_left <= depth_used)
                return min(val, depth_left);

            // Need re-solve with larger depth_left.
            uint32_t expected_pack = packed;
            uint32_t new_pack = (expected_pack & ~0xFF) | 1; // set state to 1
            if (memo_table[idx].compare_exchange_weak(expected_pack, new_pack,
                                                      memory_order_acq_rel,
                                                      memory_order_acquire)) {
                owns_slot = true;
                break;
            }
            continue;
        }
        if (st == 1) {
            // Another thread is computing this key.
            // Fall back to local computation without claiming/writing the slot. 
            // With round-invariant keys (depth not in key), waiting here can deadlock across threads (A waits on B while B waits on A).
            break;
        }
        uint32_t expected = 0;
        if (memo_table[idx].compare_exchange_weak(expected, 1, memory_order_acq_rel, memory_order_acquire)) {
            owns_slot = true;
            break; // this thread computes
        }
    }

    states_computed.fetch_add(1, memory_order_relaxed);

    int result = 0;

    if (is_x_turn) {
        // distance pruning
        int min_dist = min_dist_to_dets(x_pos, dets, nd);
        int best_move = x_pos;
        if (min_dist > depth_left) {
            result = depth_left;
            best_move = adj[x_pos].empty() ? x_pos : adj[x_pos][0];
        } else {
            const auto &moves = adj[x_pos];
            if (moves.empty()) {
                result = 0;
                best_move = x_pos;
            } else {
                vector<int> ordered = moves;
                sort(ordered.begin(), ordered.end(), [&](int a, int b) {
                    int da = min_dist_to_dets(a, dets, nd);
                    int db = min_dist_to_dets(b, dets, nd);
                    return da > db;
                });

                int best = -1;
                int nm = (int)ordered.size();
                best_move = ordered[0];

                if (allow_root_parallel && NUM_THREADS > 1 && nm > 1) {
                    int workers = min(NUM_THREADS, nm);
                    atomic<int> next_idx(0);
                    vector<pair<int,int>> local_best(workers, {-1, ordered[0]});
                    vector<thread> threads;
                    threads.reserve(workers);

                    for (int t = 0; t < workers; ++t) {
                        threads.emplace_back([&, t]() {
                            int dets_local[5];
                            for (int j = 0; j < nd; ++j) dets_local[j] = dets[j];

                            int lb = -1;
                            while (true) {
                                int i = next_idx.fetch_add(1, memory_order_relaxed);
                                if (i >= nm) break;
                                int mv = ordered[i];
                                int child = solve(depth_left - 1, mv, dets_local, nd, false, false);
                                int survival = 1 + child;
                                if (survival > lb) {
                                    lb = survival;
                                    local_best[t].second = mv;
                                }
                            }
                            local_best[t].first = lb;
                        });
                    }
                    for (auto &th : threads) th.join();

                    for (auto &p : local_best) {
                        if (p.first > best) {
                            best = p.first;
                            best_move = p.second;
                        }
                    }
                } else {
                    for (int mv : ordered) {
                        int child = solve(depth_left - 1, mv, dets, nd, false, false);
                        int survival = 1 + child;
                        if (survival > best) {
                            best = survival;
                            best_move = mv;
                        }
                        if (best >= depth_left) break;
                    }
                }

                if (best < 0) best = 0;
                if (best > depth_left) best = depth_left;
                result = best;
            }
        }

        if (owns_slot) {
            mrx_best_move[idx] = (uint8_t)best_move;
            mrx_policy_set[idx] = 1;
        }
    } else {
        // detectives turn (minimizer)
        DetRecurseCtx ctx;
        ctx.depth_left = depth_left;
        ctx.x_pos = x_pos;
        ctx.nd = nd;
        ctx.worst_depth = depth_left + 1;
        ctx.found = false;
        for (int i = 0; i < nd; ++i) ctx.orig[i] = dets[i];

        det_recurse(ctx, 0);
        result = (ctx.worst_depth > depth_left) ? depth_left : ctx.worst_depth;

        if (owns_slot && ctx.found) {
            det_policy_set[idx] = 1;
            size_t base = idx * (size_t)MAX_DETS;
            for (int i = 0; i < nd; ++i)
                det_policy_flat[base + i] = (uint8_t)ctx.worst_combo[i];
        }
    }

    if (owns_slot) {
        uint32_t final_pack = 2 | ((uint32_t)result << 8) | ((uint32_t)depth_left << 16);
        memo_table[idx].store(final_pack, memory_order_release);
    }
    return result;
}

static string json_escape(const string &s) {
    string out;
    for (char c : s) {
        if (c == '"')       out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else                out += c;
    }
    return out;
}

static string state_to_key(bool is_x_turn, int x_pos, const int* dets, int nd) {
    string k = "p=";
    k += (is_x_turn ? "mrx" : "detectives");
    k += "|x=" + to_string(x_pos) + "|d=";
    for (int i = 0; i < nd; ++i) {
        if (i) k += ',';
        k += to_string(dets[i]);
    }
    return k;
}

static void enumerate_det_tuples_rec(int idx, int lo, int *buf,
                                     const function<void(const int*)> &fn) {
    if (idx == ND) {
        fn(buf);
        return;
    }
    for (int v = lo; v <= num_nodes; ++v) {
        buf[idx] = v;
        enumerate_det_tuples_rec(idx + 1, v, buf, fn);
    }
}

static void build_json_maps() {
    sorted_mrx.clear();
    sorted_det.clear();
    sorted_survival.clear();

    int dets[5];
    enumerate_det_tuples_rec(0, 1, dets, [&](const int* tuple) {
        for (int x = 1; x <= num_nodes; ++x) {
            size_t idx_x = state_index(x, tuple, ND, true);
            uint32_t pk_x = memo_table[idx_x].load(memory_order_acquire);
            if ((pk_x & 0xFF) == 2 && mrx_policy_set[idx_x]) {
                string key = state_to_key(true, x, tuple, ND);
                sorted_survival[key] = (pk_x >> 8) & 0xFF;
                sorted_mrx[key] = (int)mrx_best_move[idx_x];
            }

            size_t idx_d = state_index(x, tuple, ND, false);
            uint32_t pk_d = memo_table[idx_d].load(memory_order_acquire);
            if ((pk_d & 0xFF) == 2 && det_policy_set[idx_d]) {
                string key = state_to_key(false, x, tuple, ND);
                sorted_survival[key] = (pk_d >> 8) & 0xFF;
                vector<int> mv;
                mv.reserve(ND);
                size_t base = idx_d * (size_t)MAX_DETS;
                for (int i = 0; i < ND; ++i)
                    mv.push_back((int)det_policy_flat[base + i]);
                sorted_det[key] = mv;
            }
        }
    });
}

static void write_json(const string &path,
                       const string &map_path,
                       int mrx_start,
                       const vector<int> &det_starts,
                       int guaranteed_survival,
                       bool forced_escape,
                       double solve_time_s,
                       uint64_t states_eval)
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
    f << "    \"detective_policy_size\": " << sorted_det.size() << ",\n";
    f << "    \"forced_escape\": " << (forced_escape ? "true" : "false") << ",\n";
    f << "    \"memo_size_positions\": " << memo_table_size << ",\n";
    f << "    \"policy_size\": " << sorted_mrx.size() << ",\n";
    f << "    \"solve_time_seconds\": " << solve_time_s << ",\n";
    f << "    \"states_evaluated\": " << states_eval << "\n";
    f << "  },\n";

    f << "  \"survival\": {\n";
    {
        bool first = true;
        for (auto &[k, v] : sorted_survival) {
            if (!first) f << ",\n";
            f << "    \"" << json_escape(k) << "\": " << v;
            first = false;
        }
    }
    f << "\n  }\n";

    f << "}\n";
    f.close();
    cout << "Policy written to: " << path << "\n";
}

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
                         uint64_t states_eval)
{
    int nd = (int)det_starts.size();
    int mrx_rec_len = nd + 3;
    int det_rec_len = 2 * nd + 2;

    struct MrxRec {
        uint8_t data[8];
        bool operator<(const MrxRec &o) const { return memcmp(data, o.data, sizeof(data)) < 0; }
    };
    struct DetRec {
        uint8_t data[12];
        bool operator<(const DetRec &o) const { return memcmp(data, o.data, sizeof(data)) < 0; }
    };

    vector<MrxRec> mrx_recs;
    mrx_recs.reserve(sorted_mrx.size());
    vector<DetRec> det_recs;
    det_recs.reserve(sorted_det.size());

    int dets[5];
    enumerate_det_tuples_rec(0, 1, dets, [&](const int* tuple) {
        for (int x = 1; x <= num_nodes; ++x) {
            size_t idx_x = state_index(x, tuple, nd, true);
            uint32_t pk_x = memo_table[idx_x].load(memory_order_acquire);
            if ((pk_x & 0xFF) == 2 && mrx_policy_set[idx_x]) {
                MrxRec r{};
                r.data[0] = (uint8_t)x;
                for (int i = 0; i < nd; ++i) r.data[1 + i] = (uint8_t)tuple[i];
                r.data[1 + nd] = mrx_best_move[idx_x];
                r.data[2 + nd] = (pk_x >> 8) & 0xFF;
                mrx_recs.push_back(r);
            }

            size_t idx_d = state_index(x, tuple, nd, false);
            uint32_t pk_d = memo_table[idx_d].load(memory_order_acquire);
            if ((pk_d & 0xFF) == 2 && det_policy_set[idx_d]) {
                DetRec r{};
                r.data[0] = (uint8_t)x;
                for (int i = 0; i < nd; ++i) r.data[1 + i] = (uint8_t)tuple[i];
                size_t base = idx_d * (size_t)MAX_DETS;
                for (int i = 0; i < nd; ++i) r.data[1 + nd + i] = det_policy_flat[base + i];
                r.data[1 + 2 * nd] = (pk_d >> 8) & 0xFF;
                det_recs.push_back(r);
            }
        }
    });

    sort(mrx_recs.begin(), mrx_recs.end());
    sort(det_recs.begin(), det_recs.end());

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
    hdr << "    \"policy_size\": " << mrx_recs.size() << ",\n";
    hdr << "    \"detective_policy_size\": " << det_recs.size() << ",\n";
    hdr << "    \"memo_size_positions\": " << memo_table_size << ",\n";
    hdr << "    \"states_evaluated\": null,\n";
    hdr << "    \"solve_time_seconds\": null,\n";
    hdr << "    \"forced_escape\": " << (forced_escape ? "true" : "false") << "\n";
    hdr << "  },\n";
    hdr << "  \"binary_layout\": {\n";
    hdr << "    \"mrx_record_bytes\": " << mrx_rec_len << ",\n";
    hdr << "    \"det_record_bytes\": " << det_rec_len << "\n";
    hdr << "  }\n";
    hdr << "}";
    string header_str = hdr.str();

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
    cout << "Binary policy written to: " << path << "\n";
}

static int best_first_move(int depth_left, int x_pos, int* dets, int nd) {
    const auto &moves = adj[x_pos];
    if (moves.empty()) return x_pos;

    vector<int> ordered = moves;
    sort(ordered.begin(), ordered.end(), [&](int a, int b) {
        int da = min_dist_to_dets(a, dets, nd);
        int db = min_dist_to_dets(b, dets, nd);
        return da > db;
    });

    int best_move = ordered[0];
    int best_survival = -1;
    for (int mv : ordered) {
        int dets_local[5];
        for (int i = 0; i < nd; ++i) dets_local[i] = dets[i];
        int child = solve(depth_left - 1, mv, dets_local, nd, false, false);
        int s = 1 + child;
        if (s > best_survival) {
            best_survival = s;
            best_move = mv;
        }
        if (best_survival >= depth_left) break;
    }
    return best_move;
}

int main(int argc, char *argv[]) {
    int mrx_start = -1;
    vector<int> det_starts;
    string map_path;
    string output_format = "binary";
    string file_suffix;

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
        } else if (!strcmp(argv[i], "--threads") && i + 1 < argc) {
            NUM_THREADS = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--output-format") && i + 1 < argc) {
            output_format = argv[++i];
            if (output_format != "json" && output_format != "binary" && output_format != "both") {
                cerr << "Error: --output-format must be 'json', 'binary', or 'both'.\n";
                return 1;
            }
        } else if (!strcmp(argv[i], "--suffix") && i + 1 < argc) {
            file_suffix = argv[++i];
        }
    }

    if (mrx_start < 0 || det_starts.empty() || map_path.empty()) {
        cerr << "Usage: " << argv[0]
             << " --map <path> --mrx <node> --detectives <d1> <d2> ..."
               << " [--max-rounds <N>] [--threads <T>]"
             << " [--output-format json|binary|both]"
             << " [--suffix <tag>]\n";
        return 1;
    }

    if (NUM_THREADS < 1) {
        cerr << "Error: --threads must be >= 1.\n";
        return 1;
    }

    ND = (int)det_starts.size();
    if (ND > MAX_DETS) {
        cerr << "Error: at most " << MAX_DETS << " detectives supported.\n";
        return 1;
    }

    read_map(map_path);

    auto node_ok = [](int n) { return n >= 1 && n <= num_nodes && !adj[n].empty(); };
    if (!node_ok(mrx_start)) {
        cerr << "Error: Mr. X node " << mrx_start << " not on board.\n";
        return 1;
    }
    for (int d : det_starts) {
        if (!node_ok(d)) {
            cerr << "Error: detective node " << d << " not on board.\n";
            return 1;
        }
    }
    {
        set<int> all(det_starts.begin(), det_starts.end());
        all.insert(mrx_start);
        if ((int)all.size() != ND + 1) {
            cerr << "Error: starting positions must be distinct.\n";
            return 1;
        }
    }

    sort(det_starts.begin(), det_starts.end());

    cout << "Map: " << map_path << "  (" << num_nodes << " nodes)\n";
    cout << "Mr. X: " << mrx_start << "  |  Detectives:";
    for (int d : det_starts) cout << " " << d;
    cout
         << "  |  Max rounds: " << MAX_ROUNDS
         << "  |  Threads: " << NUM_THREADS << "\n";

    cout << "BFS distances..." << flush;
    auto t_bfs0 = chrono::high_resolution_clock::now();
    precompute_distances();
    auto t_bfs1 = chrono::high_resolution_clock::now();
    double bfs_s = chrono::duration<double>(t_bfs1 - t_bfs0).count();
    cout << " done. (" << bfs_s << " s)\n";

    build_nCk();
    num_det_states = (size_t)nCk[num_nodes + ND - 1][ND];

    size_t total_states = 2ull
                        * (size_t)(num_nodes + 1)
                        * num_det_states;
    memo_table_size = total_states;

    if (total_states == 0 || total_states > MAX_TOTAL_STATES) {
        cerr << "Error: dense state table too large (" << total_states
             << "). Reduce detectives/rounds or use sparse solver.\n";
        return 1;
    }

    memo_table = make_unique<atomic<uint32_t>[]>(total_states);
    mrx_best_move.assign(total_states, 0);
    mrx_policy_set.assign(total_states, 0);
    det_policy_flat.assign(total_states * (size_t)MAX_DETS, 0);
    det_policy_set.assign(total_states, 0);
    for (size_t i = 0; i < total_states; ++i)
        memo_table[i].store(0, memory_order_relaxed);

    cout << "Dense memo states: " << total_states << "\n";
    cout << "Approx memo memory: "
            << (double)(total_states * sizeof(uint32_t)) / (1024.0 * 1024.0)
         << " MiB\n";

    states_computed.store(0, memory_order_relaxed);

    int dets_arr[5];
    for (int i = 0; i < ND; ++i) dets_arr[i] = det_starts[i];

    auto t0 = chrono::high_resolution_clock::now();
    int guaranteed = solve(MAX_ROUNDS, mrx_start, dets_arr, ND, true, true);
    auto t1 = chrono::high_resolution_clock::now();
    double solve_s = chrono::duration<double>(t1 - t0).count();

    int first_move = best_first_move(MAX_ROUNDS, mrx_start, dets_arr, ND);
    bool forced_escape = (guaranteed >= MAX_ROUNDS);

    cout << "\n=== Policy Solve (C++ dense) ===\n";
    cout << "BFS time: " << bfs_s << " s\n";
    cout << "Solve time: " << solve_s << " s\n";
    cout << "States computed: " << states_computed.load(memory_order_relaxed) << "\n";
    cout << "Forced escape: " << (forced_escape ? "YES" : "NO") << "\n";
    cout << "Recommended first move for Mr. X: " << first_move << "\n";

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

    filesystem::create_directories("policies");
    string stem = "policies/" + map_name + "_x" + to_string(mrx_start)
                + "_d" + det_str + "_r" + to_string(MAX_ROUNDS) + "_cpp_dense";
    if (!file_suffix.empty()) stem += "_" + file_suffix;

    bool emit_json   = (output_format == "json" || output_format == "both");
    bool emit_binary = (output_format == "binary" || output_format == "both");

    if (emit_json) {
        cout << "Building policy maps..." << flush;
        build_json_maps();
        cout << " done. (" << sorted_mrx.size() << " mrx, "
             << sorted_det.size() << " det entries)\n";
    }

    uint64_t evaluated = states_computed.load(memory_order_relaxed);
    auto t_store0 = chrono::high_resolution_clock::now();
    if (emit_json) {
        string json_path = stem + ".json";
        write_json(json_path, map_path, mrx_start, det_starts,
                   guaranteed, forced_escape, solve_s, evaluated);
    }
    if (emit_binary) {
        string bin_path = stem + ".bin";
        write_binary(bin_path, map_path, mrx_start, det_starts,
                     guaranteed, forced_escape, solve_s, evaluated);
    }
    auto t_store1 = chrono::high_resolution_clock::now();
    double store_s = chrono::duration<double>(t_store1 - t_store0).count();
    cout << "Policy store time: " << store_s << " s\n";

    return 0;
}
