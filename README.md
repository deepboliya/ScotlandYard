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

### Observer Mode (watch AI play)

```bash
python main.py
python main.py --map first100 --mrx 13 --detectives 7 43 --max-rounds 24
```

Controls: **N** step | **R** round | **A** auto-play | **Q** quit.

### Play as Mr. X

```bash
python main.py --mode play-mrx
```

Click green-highlighted nodes to move. Detectives move automatically.

### Play as Detectives

```bash
# Against a stored Mr. X policy
python main.py --mode play-detective --policy-file x13_d7_43_r24_cpp.json

# Without policy file (Mr. X plays randomly)
python main.py --mode play-detective --mrx 1 --detectives 5 10
```

Click green nodes for each detective in turn.

### Text-Only Mode

```bash
python main.py --no-viz --policy-file x13_d7_43_r24_cpp.json
```

### Solver Mode (exhaustive minimax)

```bash
# Python solver
python main.py --mode solve --mrx 13 --detectives 7 43 --max-rounds 24 --dump-policy

# C++ solver (faster)
g++ -O2 -std=c++17 -o solve solver/solve.cpp
./solve --map maps/first50.txt --mrx 13 --detectives 7 43 --max-rounds 24

# C++ sparse policy solver (recommended default)
g++ -O3 -std=c++17 -o fast_policy_v2 fast_policy_v2.cpp
./fast_policy_v2 --map maps/full_map.txt --mrx 100 --detectives 1 199 --max-rounds 12 --output-format binary

# C++ dense policy solver (fixed arrays, optional threading)
g++ -O3 -std=c++17 -pthread -o fast_policy_v2_1 fast_policy_v2.1.cpp
./fast_policy_v2_1 --map maps/first50.txt --mrx 13 --detectives 7 20 43 --max-rounds 5 --threads 4
```

Both produce a JSON policy file (e.g. `x13_d7_43_r24.json` or
`x13_d7_43_r24_cpp.json`) that can be loaded with `--policy-file`.

Dense solver notes:

- `fast_policy_v2.1.cpp` supports detective counts from 1 to 5.
- It uses a dense fixed-size state table, so memory grows quickly with
  map size, rounds, and number of detectives.
- If the dense table would be too large, it exits with an error; use
  `fast_policy_v2.cpp` for larger configurations.

### Hint Overlay

```bash
python main.py --mode play-mrx --policy-file x13_d7_43_r24_cpp.json --help-human
```

Highlights the solver-optimal move in light yellow while you play.

## CLI Reference

| Flag | Default | Description |
|------|---------|-------------|
| `--mode` | `auto` | `auto`, `play-mrx`, `play-detective`, `solve` |
| `--map` | `first50` | Map name in `maps/` (without `.txt`) |
| `--mrx` | `1` | Mr. X starting node |
| `--detectives` | `5 10` | Detective starting nodes |
| `--max-rounds` | `15` | Rounds before Mr. X wins |
| `--seed` | — | Random seed for reproducibility |
| `--no-viz` | — | Text-only mode (no GUI) |
| `--dump-policy` | — | Write solver output to JSON |
| `--policy-file` | — | Load policy + config from JSON |
| `--help-human` | — | Highlight optimal moves (requires `--policy-file`) |

## Policy JSON Format

Policy files (`scotlandyard-policy-v2`) contain everything needed to
replay or analyse a solved configuration:

```json
{
  "format": "scotlandyard-policy-v2",
  "board": "maps/first50.txt",
  "config": {
    "mrx_start": 13,
    "detective_starts": [7, 43],
    "max_rounds": 24
  },
  "policy": {
    "r=0|p=mrx|x=13|d=7,43": 4
  },
  "detective_policy": {
    "r=1|p=detectives|x=4|d=7,43": [6, 18]
  },
  "survival_depths": {
    "r=0|p=mrx|x=13|d=7,43": 10
  },
  "solver": {
    "forced_escape": false,
    "guaranteed_survival_depth": 10,
    "states_evaluated": 1873613,
    "policy_size": 949405,
    "detective_policy_size": 858086,
    "survival_depths_size": 1873613,
    "solve_time_seconds": 5.07
  }
}
```

- **`policy`**: maps each Mr. X state to the optimal next node.
- **`detective_policy`**: maps each detective state to a list of target nodes.
- **`survival_depths`**: maps every visited state to the guaranteed survival
  depth (the round at which Mr. X is caught under optimal play, or
  `max_rounds` if he escapes). Loaded by the visualiser and move logger
  to display `SD=10(+9)` annotations without re-running the solver.
- When `--policy-file` is used, `config` overrides `--mrx`/`--detectives`/`--max-rounds`.

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

## Adding a Custom Strategy

```python
from strategies.base import Strategy

class MyStrategy(Strategy):
    def choose_move(self, board, state, player_id, valid_moves):
        return best_node
```

Wire it into the engine:

```python
engine = GameEngine(board, state,
                    mrx_strategy=MyStrategy(),
                    detective_strategy=MyStrategy())
```
