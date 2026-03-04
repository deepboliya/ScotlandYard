#include <iostream>
#include <vector>
#include <atomic>
#include <thread>
#include <queue>
#include <algorithm>
#include <chrono>
#include <set>
#include <map>
#include <functional>

using namespace std;

// ==============================================================================
// 1. CONSTANTS & GLOBALS
// ==============================================================================
const int MAX_DETS = 5;
const int MAX_TURNS = 5;
const int MAX_MAP_SIZE = 250;
const int NUM_NODES = 100;

vector<vector<int>> graph(MAX_MAP_SIZE);
int dist_matrix[MAX_MAP_SIZE][MAX_MAP_SIZE];

atomic<bool> global_win_found{ false };

const uint64_t TT_SIZE = 1ULL << 24;
const uint64_t TT_MASK = TT_SIZE - 1;
atomic<uint64_t>* TT;

// ==============================================================================
// 2. GRAPH & DISTANCES
// ==============================================================================
void build_graph() {
    vector<pair<int, int>> edges = {
        {1,8}, {1,9}, {1,46}, {1,58}, {2,10}, {2,20}, {3,4}, {3,11}, {3,12},
        {3,22}, {3,23}, {4,13}, {5,15}, {5,16}, {6,7}, {6,29}, {7,17}, {7,42},
        {8,18}, {8,19}, {9,19}, {9,20},{10,11}, {10,21}, {10,34}, {11,22}, {12,23},
        {13,14}, {13,23},{13,24}, {13,46},{13,67}, {13,89}, {14,15}, {14,25}, {15,16}, {15,26},{15,28},{15,29},
        {15,41}, {16,28}, {16,29}, {17,29}, {17,30}, {18,31}, {18,43}, {19,32}, {20,33},
        {21,33}, {22,23}, {22,34},{22,35},{22,65}, {23,37}, {24,37}, {24,38}, {25,38}, {25,39},
        // Nodes 26–50:
         {26,27}, {26,39}, {27,28}, {27,40}, {28,41}, {28,42},{29,41},{29,55},{29,42},
        {30,42},{31,43}, {31,44}, {31,32},{32,33}, {32,44}, {32,45}, {33,46}, {34,46}, {34,47},{34,48},{34,63}, {35,36},
        {35,48},{35,65}, {36,37}, {36,49}, {37,50}, {38,50}, {38,51}, {39,51}, {39,52},
        {40,41}, {40,52},{40,53}, {41,52},{41,54},{41,87}, {42,56}, {42,72}, {43,57}, {44,58},
        {45,46},{45,58}, {45,59},{45,60}, {46,47},{46,58}, {46,61},{46,78},{46,79 }, {47, 62}, { 48,62 }, { 48,63 },
        {49,50}, {49,66},
        // Nodes 51–75:
        {51,52}, {51,67},{51,68},{52,67},
        
        
        {52,69}, {52,86}, {53,54}, {53,69}, {54,55}, {54,70},
        {55,71}, {56,91},{57,58}, {57,73}, {58,59}, {58,74},{58,77}, {59,75},
        {59,76}, {60,61}, {60,76}, {61,62}, {61,76}, {61,78}, {62,79}, {63,64},
        {63,79},{63,80},{63,100}, {64,65}, {64,81}, {65,66},{65,67}, {65,82}, {66,67}, {66,82}, {67,68},
        {67,82},{67,84},{67,102},{67,89},{67,79},{67,111}, {68,69}, {68,85}, {69,86}, {70,71}, {70,87}, {71,72},
        {71,89}, {72,90}, {72,91},{72,107},{72,105}, {73,74}, {73,92}, {74,75},{74,92},{74,94}, {75,94},
        // Nodes 76–100:
        {76,77}, {77,78}, {77,95}, {77,96},{77,124}, {78,79}, {78,97}, {79,98}, {79,111},
        {80,99}, {80,100}, {81,82}, {81,100}, {82,100}, {82,101},{82,140}, {83,101}, {83,102},
        {84,85}, {85,103}, {85,100}, {86,87}, {86,102}, {86,103},{86,104},{86,116}, {87,88}, {87,89},
        {88,89}, {88,117}, {89,90}, {89,105},{89,128},{89,140}, {90,91}, {90,105}, {91,105}, {91,107},
        {92,93}, {93,94},  {94,95}, {95,122}, 
        {96,97}, {96,109}, {97,98}, {97,109}, {98,99}, {98,110}, {99,110}, {99,112},
        {100,101}, {100,112},{100,111},{100,113},/*
        // Nodes 101–125:
        {101,102}, {101,116}, {102,103}, {102,117}, {103,104}, {103,118}, {104,105},
        {104,119}, {105,106}, {105,120}, {106,107}, {106,121}, {107,108}, {107,122},
        {108,109}, {108,123}, {109,110}, {109,124}, {110,111}, {110,125}, {111,112},
        {111,126}, {112,113}, {112,127}, {113,114}, {113,128}, {114,115}, {114,129},
        {115,116}, {115,130}, {116,117}, {116,131}, {117,118}, {117,132}, {118,119},
        {118,133}, {119,120}, {119,134}, {120,121}, {120,135}, {121,122}, {121,136},
        {122,123}, {122,137}, {123,124}, {123,138}, {124,125}, {124,139}, {125,126},
        {125,140},
        // Nodes 126–150:
        {126,127}, {126,141}, {127,128}, {127,142}, {128,129}, {128,143}, {129,130},
        {129,144}, {130,131}, {130,145}, {131,132}, {131,146}, {132,133}, {132,147},
        {133,134}, {133,148}, {134,135}, {134,149}, {135,136}, {135,150}, {136,137},
        {136,151}, {137,138}, {137,152}, {138,139}, {138,153}, {139,140}, {139,154},
        {140,141}, {140,155}, {141,142}, {141,156}, {142,143}, {142,157}, {143,144},
        {143,158}, {144,145}, {144,159}, {145,146}, {145,160}, {146,147}, {146,161},
        {147,148}, {147,162}, {148,149}, {148,163}, {149,150}, {149,164}, {150,151},
        {150,165},
        // Nodes 151–175:
        {151,152}, {151,166}, {152,153}, {152,167}, {153,154}, {153,168}, {154,155},
        {154,169}, {155,156}, {155,170}, {156,157}, {156,171}, {157,158}, {157,172},
        {158,159}, {158,173}, {159,160}, {159,174}, {160,161}, {160,175}, {161,162},
        {161,176}, {162,163}, {162,177}, {163,164}, {163,178}, {164,165}, {164,179},
        {165,166}, {165,180}, {166,167}, {166,181}, {167,168}, {167,182}, {168,169},
        {168,183}, {169,170}, {169,184}, {170,171}, {170,185}, {171,172}, {171,186},
        {172,173}, {172,187}, {173,174}, {173,188}, {174,175}, {174,189}, {175,176},
        // Nodes 176–199:
        {176,177}, {176,191}, {177,178}, {177,192}, {178,179}, {178,193}, {179,180},
        {179,194}, {180,181}, {180,195}, {181,182}, {181,196}, {182,183}, {182,197},
        {183,184}, {183,198}, {184,185}, {184,199}, {185,186}, {186,187}, {187,188},
        {188,189}, {189,190}, {190,191}, {191,192}, {192,193}, {193,194}, {194,195},
        {195,196}, {196,197}, {197,198}, {198,199}*/
    };
    for (auto& e : edges) {
        if (e.first <= NUM_NODES && e.second <= NUM_NODES) {
            graph[e.first].push_back(e.second);
            graph[e.second].push_back(e.first);
        }
    }
}

void precompute_distances() {
    for (int i = 1; i <= NUM_NODES; ++i) {
        for (int j = 1; j <= NUM_NODES; ++j) dist_matrix[i][j] = 999;
        dist_matrix[i][i] = 0;
        queue<int> q;
        q.push(i);
        while (!q.empty()) {
            int curr = q.front(); q.pop();
            for (int nb : graph[curr]) {
                if (dist_matrix[i][nb] == 999) {
                    dist_matrix[i][nb] = dist_matrix[i][curr] + 1;
                    q.push(nb);
                }
            }
        }
    }
}

// ==============================================================================
// 3. COMBINATION GENERATOR (no std::function to avoid linker issues)
// ==============================================================================
void gen_combinations(const vector<int>& pool, int k, int start, int depth,
    vector<int>& current, vector<vector<int>>& result) {
    if (depth == k) {
        result.push_back(current);
        return;
    }
    int remaining = k - depth;
    for (int i = start; i <= (int)pool.size() - remaining; ++i) {
        current[depth] = pool[i];
        gen_combinations(pool, k, i + 1, depth + 1, current, result);
    }
}

vector<vector<int>> combinations(const vector<int>& pool, int k) {
    vector<vector<int>> result;
    vector<int> current(k, 0);
    gen_combinations(pool, k, 0, 0, current, result);
    return result;
}

// ==============================================================================
// 4. STATE ENCODING
// ==============================================================================
inline uint64_t encode_state(int turn, int x_pos, const vector<int>& dets,
    bool is_x_turn, int num_dets) {
    vector<int> d(dets.begin(), dets.begin() + num_dets);
    sort(d.begin(), d.end());
    uint64_t key = (uint64_t)turn;
    key = (key << 1) | (uint64_t)is_x_turn;
    key = (key << 8) | (uint64_t)x_pos;
    key = (key << 4) | (uint64_t)num_dets;
    for (int i = 0; i < num_dets; ++i)
        key = (key << 8) | (uint64_t)d[i];
    return key;
}

inline uint64_t pack_tt_entry(uint64_t state_key, int8_t score) {
    uint64_t enc = (score == 1) ? 1ULL : 2ULL;
    return state_key | (enc << 60);
}

inline int8_t unpack_score(uint64_t entry) {
    uint64_t enc = entry >> 60;
    if (enc == 1) return  1;
    if (enc == 2) return -1;
    return 0;
}

inline uint64_t unpack_key(uint64_t entry) {
    return entry & 0x0FFFFFFFFFFFFFFF;
}

// ==============================================================================
// 5. FORWARD DECLARATIONS (both functions call each other)
// ==============================================================================
int8_t fast_minimax(int turn, int x_pos, const vector<int>& dets,
    bool is_x_turn, int max_turns, int num_dets);

bool evaluate_det_combinations(int det_idx, int turn, int x_pos,
    vector<int>& current_dets,
    int max_turns, int num_dets);

// ==============================================================================
// 6. MINIMAX SOLVER
// ==============================================================================
bool evaluate_det_combinations(int det_idx, int turn, int x_pos,
    vector<int>& current_dets,
    int max_turns, int num_dets) {
    if (global_win_found.load(memory_order_relaxed)) return true;

    if (det_idx == num_dets)
        return fast_minimax(turn + 1, x_pos, current_dets, true, max_turns, num_dets) == -1;

    int orig = current_dets[det_idx];
    vector<int> moves = graph[orig];
    sort(moves.begin(), moves.end(), [&](int a, int b) {
        return dist_matrix[a][x_pos] < dist_matrix[b][x_pos];
        });

    for (int next_d : moves) {
        current_dets[det_idx] = next_d;
        if (evaluate_det_combinations(det_idx + 1, turn, x_pos,
            current_dets, max_turns, num_dets)) {
            current_dets[det_idx] = orig;
            return true;
        }
    }
    current_dets[det_idx] = orig;
    return false;
}

int8_t fast_minimax(int turn, int x_pos, const vector<int>& dets,
    bool is_x_turn, int max_turns, int num_dets) {
    if (global_win_found.load(memory_order_relaxed)) return 1;

    uint64_t state_key = encode_state(turn, x_pos, dets, is_x_turn, num_dets);
    uint64_t tt_idx = (state_key ^ (state_key >> 17) ^ (state_key >> 34)) & TT_MASK;

    uint64_t stored = TT[tt_idx].load(memory_order_relaxed);
    if (stored != 0 && unpack_key(stored) == state_key)
        return unpack_score(stored);

    // Terminal: caught
    for (int i = 0; i < num_dets; ++i)
        if (x_pos == dets[i]) return -1;

    // Terminal: survived
    if (turn == max_turns) return 1;

    // Distance pruning
    int min_dist = 999;
    for (int i = 0; i < num_dets; ++i)
        min_dist = min(min_dist, dist_matrix[x_pos][dets[i]]);
    if (min_dist > (max_turns - turn)) return 1;

    int8_t score;

    if (is_x_turn) {
        vector<int> moves = graph[x_pos];
        sort(moves.begin(), moves.end(), [&](int a, int b) {
            int da = 999, db = 999;
            for (int i = 0; i < num_dets; ++i) {
                da = min(da, dist_matrix[a][dets[i]]);
                db = min(db, dist_matrix[b][dets[i]]);
            }
            return da > db;
            });
        score = -1;
        for (int nx : moves) {
            if (fast_minimax(turn, nx, dets, false, max_turns, num_dets) == 1) {
                score = 1;
                break;
            }
        }
    }
    else {
        vector<int> dets_copy(dets.begin(), dets.begin() + num_dets);
        score = evaluate_det_combinations(0, turn, x_pos, dets_copy,
            max_turns, num_dets) ? -1 : 1;
    }

    TT[tt_idx].store(pack_tt_entry(state_key, score), memory_order_relaxed);
    return score;
}

// ==============================================================================
// 7. THREAD WORKER & SOLVE
// ==============================================================================
void root_worker(int x_move, vector<int> det_config, int max_turns,
    int num_dets, int& result) {
    result = fast_minimax(0, x_move, det_config, false, max_turns, num_dets);
    if (result == 1)
        global_win_found.store(true, memory_order_relaxed);
}

int solve(int x_start, const vector<int>& det_config, int num_dets) {
    global_win_found.store(false, memory_order_relaxed);

    const vector<int>& root_moves = graph[x_start];
    int n = (int)root_moves.size();

    vector<thread> workers;
    vector<int> results(n, -1);

    for (int i = 0; i < n; ++i)
        workers.emplace_back(root_worker, root_moves[i], det_config,
            MAX_TURNS, num_dets, ref(results[i]));
    for (auto& t : workers) t.join();

    for (int r : results) if (r == 1) return 1;
    return -1;
}

// ==============================================================================
// 8. SUBSET PRUNING
// Returns true if any strict subset of det_config already beats X at x_start
// ==============================================================================
bool any_subset_is_loss(const vector<int>& config,
    const set<vector<int>>& losing_configs) {
    int n = (int)config.size();
    // Only check proper subsets (mask != full set)
    int full_mask = (1 << n) - 1;
    for (int mask = 1; mask < full_mask; ++mask) {
        vector<int> subset;
        for (int i = 0; i < n; ++i)
            if (mask & (1 << i))
                subset.push_back(config[i]);
        if (losing_configs.count(subset)) return true;
    }
    return false;
}

// ==============================================================================
// 9. MAIN
// ==============================================================================
int main() {
    build_graph();
    precompute_distances();

    TT = new atomic<uint64_t>[TT_SIZE];
    for (size_t i = 0; i < TT_SIZE; ++i)
        TT[i].store(0, memory_order_relaxed);

    vector<int> all_positions = { 1,56,27,17,30,6,27 };
    // For each X position: the set of det configs (sorted) that beat it
    map<int, set<vector<int>>> x_losing_to;

    int total_solves = 0;
    int total_skipped = 0;

    cout << "============================================================\n";
    cout << "  Scotland Yard Full Sweep\n";
    cout << "  Pool size: " << all_positions.size() << " positions"
        << "  |  Max dets: " << MAX_DETS
        << "  |  Max turns: " << MAX_TURNS << "\n";
    cout << "============================================================\n";

    auto global_start = chrono::high_resolution_clock::now();

    for (int num_dets = 1; num_dets <= MAX_DETS; ++num_dets) {
        auto det_configs = combinations(all_positions, num_dets);

        cout << "\n==============================\n";
        cout << "Round: " << num_dets << " detective(s)  |  "
            << det_configs.size() << " configs to check\n";
        cout << "==============================\n";

        int round_solves = 0;
        int round_skipped = 0;
        int round_det_wins = 0;
        int round_x_wins = 0;

        auto round_start = chrono::high_resolution_clock::now();

        for (const auto& det_config : det_configs) {
            for (int x_start : all_positions) {

                // X cannot start on a detective's node
                bool overlap = false;
                for (int d : det_config)
                    if (d == x_start) { overlap = true; break; }
                if (overlap) { round_skipped++; continue; }

                // Cascade pruning: a subset already beats X here
                if (num_dets > 1 &&
                    any_subset_is_loss(det_config, x_losing_to[x_start])) {
                    round_skipped++;
                    continue;
                }

                // Solve this (X, det_config) pair
                auto t0 = chrono::high_resolution_clock::now();
                int result = solve(x_start, det_config, num_dets);
                auto t1 = chrono::high_resolution_clock::now();
                long long ms = chrono::duration_cast<chrono::milliseconds>(t1 - t0).count();

                round_solves++;

                if (result == -1) {
                    x_losing_to[x_start].insert(det_config);
                    round_det_wins++;
                    cout << "  X=" << x_start << " Dets=[";
                    for (int d : det_config) cout << d << " ";
                    cout << "] -> DET WIN  [" << ms << "ms]\n";
                }
                else {
                    round_x_wins++;
                    cout << "  X=" << x_start << " Dets=[";
                    for (int d : det_config) cout << d << " ";
                    cout << "] -> X WINS   [" << ms << "ms]\n";
                }
            }
        }

        auto round_end = chrono::high_resolution_clock::now();
        double round_sec = chrono::duration<double>(round_end - round_start).count();

        total_solves += round_solves;
        total_skipped += round_skipped;

        cout << "\n  Round " << num_dets << " Summary:\n"
            << "    Solves run:  " << round_solves << "\n"
            << "    Skipped:     " << round_skipped << "  (cascade pruning)\n"
            << "    Det wins:    " << round_det_wins << "\n"
            << "    X wins:      " << round_x_wins << "\n"
            << "    Round time:  " << round_sec << "s\n";
    }

    double total_sec = chrono::duration<double>(
        chrono::high_resolution_clock::now() - global_start).count();

    cout << "\n============================================================\n";
    cout << "FINAL RESULTS\n";
    cout << "Total solves:  " << total_solves << "\n";
    cout << "Total skipped: " << total_skipped << "  (cascade pruning savings)\n";
    cout << "Total time:    " << total_sec << "s\n\n";

    cout << "X positions that are ALWAYS caught (by at least one det config):\n";
    for (int x : all_positions) {
        auto it = x_losing_to.find(x);
        if (it != x_losing_to.end() && !it->second.empty())
            cout << "  Node " << x << "  —  beaten by "
            << it->second.size() << " det config(s)\n";
    }

    cout << "\nX positions that survive ALL tested det configs:\n";
    bool any = false;
    for (int x : all_positions) {
        auto it = x_losing_to.find(x);
        if (it == x_losing_to.end() || it->second.empty()) {
            cout << "  Node " << x << "\n";
            any = true;
        }
    }
    if (!any) cout << "  None — detectives dominate every starting position.\n";
    cout << "============================================================\n";

    delete[] TT;
    return 0;
}