# Scotland Yard

A Scotland Yard game environment with an **exhaustive minimax solver**
for computing optimal strategies for both Mr. X and detectives.

## Project Structure

```
main.py                 CLI entry point (all game modes)
fast_policy_v2.cpp      Sparse C++ minimax solver (unordered_map memo)
fast_policy_v2.1.cpp    Dense C++ minimax solver (fixed-array memo, up to 5 detectives)
game/
  board.py              Graph representation (adjacency, positions)
  state.py              Game state (positions, rounds, turn tracking)
  engine.py             Turn engine (step / round / full game)
strategies/
  base.py               Abstract Strategy interface
  random_strategy.py    Random move selection
  human.py              Click-to-move via visualiser callback
  policy_strategy.py    Plays from a precomputed policy (in-memory or JSON)
old_solvers/
  exhaustive_solver.py  Python minimax solver with memoisation
  solve.cpp             Legacy C++ minimax solver
visualization/
  visualizer.py         Interactive matplotlib + networkx GUI
maps/
  first50.txt           50 nodes, 81 edges
  first100.txt          99 nodes, 198 edges
  full_map.txt          199 nodes, 436 edges
  node_locations.csv    Node coordinates for visualisation layout
```

## Requirements

- Python 3.10+
- `matplotlib`, `networkx`
- C++17 compiler (for the C++ solver, optional)

```bash
pip install -r requirements.txt
```

## Game Rules

- Mr. X moves first each round, then all detectives move simultaneously.
- A detective landing on Mr. X's node captures him (detectives win).
- Mr. X can move to any adjacent node, including detective-occupied nodes
  (capture is checked after the move).
- Detectives can share nodes with each other.
- If Mr. X survives `max_rounds` rounds, he wins.

## Usage

### Quick Start

```bash
# Interactive play with policy files (required)
python main.py --mrx-policy policy.bin --det-policy policy.bin

# Text-only mode (no GUI)
python main.py --mrx-policy policy.bin --det-policy policy.bin --no-viz
```

Both `--mrx-policy` and `--det-policy` are required. They can point to the
same `.bin` file if it contains both policies.

### Interactive Mode (default)

```bash
python main.py --mrx-policy final.bin --det-policy final.bin
```

Click green-highlighted nodes to move for either side.

Controls: **N** play best move | **Q** quit.

### Text-Only Mode

```bash
python main.py --mrx-policy final.bin --det-policy final.bin --no-viz
```

Runs the game fully automatically using the loaded policies. No interactive
input is possible—both sides play optimally until the game ends.

### Solver Mode (exhaustive minimax)

```bash
# Single configuration (specific Mr. X and detective positions)
g++ -O3 -std=c++17 -pthread -o fast_policy_v2_2 fast_policy_v2.2.cpp
./fast_policy_v2_2 --map maps/first50.txt --mrx 13 --detectives 7 43 --max-rounds 10 --threads 4

# Full sweep (all Mr. X starting positions)
g++ -O3 -std=c++17 -pthread -o full_sweep full_sweep.cpp
./full_sweep --map maps/full_map.txt --num-detectives 2 --max-rounds 15 --threads 8
```

See [C++ Solver Variants](#c-solver-variants) and [Full Sweep](#full-sweep) for details.

## CLI Reference

| Flag | Default | Description |
|------|---------|-------------|
| `--mrx-policy` | — | **Required.** Mr. X policy `.bin` file |
| `--det-policy` | — | **Required.** Detective policy `.bin` file |
| `--map` | `first50` | Map name in `maps/` (without `.txt`) |
| `--mrx` | `1` | Mr. X starting node |
| `--detectives` | `5 10` | Detective starting nodes |
| `--max-rounds` | `15` | Rounds before Mr. X wins |
| `--seed` | — | Random seed for reproducibility |
| `--no-viz` | — | Text-only mode (no GUI) |

## Solver Details

### What is computed

The solver determines whether Mr. X has a strategy that guarantees
escape against all possible detective strategies. Formally, it checks:

> Does there exist a Mr. X policy such that for every detective policy,
> Mr. X survives all rounds?

### Algorithm

Minimax over the full game tree with memoisation:

1. **Terminal checks**: detective on Mr. X → loss; survived `max_rounds` → win.
2. **Mr. X turn** (maximise): pick the move with the highest survival depth.
3. **Detective turn** (minimise): enumerate all joint detective placements
   (Cartesian product, deduplicated by sorted positions), pick the one
   that minimises Mr. X's survival depth.
4. **Memoisation**: states are keyed by `(round, player, mrx_pos, det_positions)`.

### Output

- `Forced escape: YES` → policy guarantees Mr. X survives all rounds.
- `Forced escape: NO` → detectives can always catch Mr. X; policy
  maximises survival depth as a best-effort fallback.

## C++ Solver Variants

Three solver implementations are provided, each with different trade-offs:

| Solver | Memo Strategy | Threading | Best For |
|--------|---------------|-----------|----------|
| `fast_policy_v2.cpp` | Hash map (`unordered_map`) | No | Large state spaces, memory-constrained |
| `fast_policy_v2.1.cpp` | Dense array (atomics) | Yes | Fast solves, moderate state spaces |
| `fast_policy_v2.2.cpp` | Dense array (packed atomics) | Yes | Thread-safe, fixes v2.1 data race |

### fast_policy_v2.cpp (Sparse)

Uses `unordered_map` for memoization. State keys are packed into `uint64_t`
(8 bits per position + 1 bit for turn). Round number is **not** part of
the key—values represent "rounds Mr. X can survive from this position."

```bash
g++ -O3 -std=c++17 -o fast_policy_v2 fast_policy_v2.cpp
./fast_policy_v2 --map maps/full_map.txt --mrx 100 --detectives 1 199 --max-rounds 12
```

**Pros:** Lower memory for sparse state spaces, no size limits.
**Cons:** Single-threaded, hash overhead.

### fast_policy_v2.1.cpp (Dense, Multi-threaded)

Uses fixed-size arrays indexed by `(depth_left, is_x_turn, x_pos, det_tuple)`.
Supports up to 5 detectives. Thread-safe via atomics (no mutexes).

```bash
g++ -O3 -std=c++17 -pthread -o fast_policy_v2_1 fast_policy_v2.1.cpp
./fast_policy_v2_1 --map maps/first50.txt --mrx 13 --detectives 7 20 43 --max-rounds 10 --threads 4
```

**Pros:** Much faster with threading, cache-friendly dense arrays.
**Cons:** Memory grows with `O(rounds × nodes × C(nodes+dets, dets))`;
exits if table exceeds 600M states.

### fast_policy_v2.2.cpp (Dense, Thread-Safe Fix)
 
Same as v2.1 but packs `memo_state`, `memo_value`, and `memo_depth_used`
into a single `atomic<uint32_t>` to eliminate a data race during
concurrent re-evaluations.

```bash
g++ -O3 -std=c++17 -pthread -o fast_policy_v2_2 fast_policy_v2.2.cpp
./fast_policy_v2_2 --map maps/first50.txt --mrx 13 --detectives 7 20 43 --max-rounds 10 --threads 8
```

**Recommended** for multi-threaded solves.

## Full Sweep

`full_sweep.cpp` exhaustively solves the game from **every possible Mr. X
starting position** against a fixed number of detectives. Unlike the
single-configuration solvers, it computes optimal play for all `N` starting
nodes in one run.

Based on `fast_policy_v2.2.cpp` with these differences:
- Iterates over all Mr. X starting positions automatically
- Drops `depth_used` from memo (pure iterative sweep)
- Displays a live progress bar tracking computed states
- Outputs a single `.bin` policy file covering all starting positions

### Usage

```bash
g++ -O3 -std=c++17 -pthread -o full_sweep full_sweep.cpp
./full_sweep --map maps/full_map.txt --num-detectives 2 --max-rounds 15 --threads 8
```

Output file: `{map_name}_d{N}_r{R}.bin` (e.g., `full_map_d2_r15.bin`)

### Options

| Flag | Default | Description |
|------|---------|-------------|
| `--map` | — | Map file path |
| `--num-detectives` | `2` | Number of detectives (1–5) |
| `--max-rounds` | `11` | Maximum rounds |
| `--threads` | `1` | Worker threads |
| `--policy-file` | auto | Custom output path |

The resulting policy file can be used with `main.py` for any Mr. X starting
position on that map.
