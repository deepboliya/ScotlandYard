# Scotland Yard — Game Environment

A simplified Scotland Yard game environment for developing and testing
**escape strategies for Mr. X**.

## Board

Uses the **top-right portion** of the real Scotland Yard board
(nodes 1–20, 20 edges).  Transport types are ignored for now — every
edge is a simple connection.

```
        1
       / \
      8   9──20──2
     / \ / \     |
   18  19   20  10─11
    |           |    \
    6─7─17     3──12
              |
              4
              |
             13─14─15─16
                    |
                    5
```

## Architecture

```
game/            Core game logic (board graph, state, engine)
strategies/      Pluggable player strategies (abstract base + impls)
visualization/   Interactive matplotlib visualiser
main.py          CLI entry point
```

Game logic and strategies are **fully decoupled** — swap in your own
strategy by implementing `strategies.base.Strategy`.

## Requirements

- Python 3.10+
- `matplotlib`
- `networkx`

Install dependencies:

```bash
pip install -r requirements.txt
```

## Usage

### 1) Visual game modes

```bash
# Watch random AIs play (press N/R/A/Q)
python main.py

# Play as Mr. X (click green nodes to move)
python main.py --mode play-mrx

# Play as detectives against Mr. X policy
python main.py --mode play-detective

# Play as detectives against a stored policy file
python main.py --mode play-detective --policy-file policy_v2.json
```

### 2) Text-only mode

```bash
# Same rules, no GUI
python main.py --no-viz
```

### 3) Solver mode (exhaustive)

```bash
# Exhaustive adversarial solve from a starting config
python main.py --mode solve --mrx 1 --detectives 5 10 --max-rounds 4

# Save solved policy as JSON
python main.py --mode solve --mrx 1 --detectives 5 10 --max-rounds 4 --dump-policy policy_v2.json
```

### 4) Custom starts

```bash
# Two detectives
python main.py --mrx 1 --detectives 5 10 --max-rounds 15

# More detectives
python main.py --mrx 9 --detectives 3 6 14
```

## Policy JSON Format (`scotlandyard-policy-v2`)

Dumped files include both solved policy and exact solve configuration:

```json
{
  "format": "scotlandyard-policy-v2",
  "board": "top-right-simple-v1",
  "config": {
    "mrx_start": 1,
    "detective_starts": [5, 10],
    "max_rounds": 4
  },
  "solver": {
    "forced_escape": true,
    "states_evaluated": 630,
    "policy_size": 113
  },
  "policy": {
    "r=0|p=mrx|x=1|d=5,10": 8
  }
}
```

When `--policy-file` is used, startup configuration is loaded from the
JSON `config` block.

If you explicitly pass `--mrx`, `--detectives`, or `--max-rounds` together
with `--policy-file`, they must exactly match the JSON config or the
program exits with an error.

`--policy-file` is not allowed with `--mode play-mrx` because in that mode
you control Mr. X directly.

## Controls

| Key | Action |
|-----|--------|
| **N** | Step — one player's move |
| **R** | Round — Mr. X + all detectives |
| **A** | Auto-play to completion |
| **Q** | Quit |

In **play-mrx** mode, click on green-highlighted nodes to move Mr. X.

## Adding a Custom Strategy

```python
from strategies.base import Strategy

class MyBrilliantStrategy(Strategy):
    def choose_move(self, board, state, player_id, valid_moves):
        # your logic here
        return best_node
```

Then wire it into the engine:

```python
engine = GameEngine(board, state,
                    mrx_strategy=MyBrilliantStrategy(),
                    detective_strategies=[...])
```

## Game Rules (simplified)

* Mr. X moves first each round, then every detective moves in order.
* Detectives **cannot** share a node with each other.
* Detectives **can** land on Mr. X's node → Mr. X is caught.
* Mr. X **cannot** move to a detective's node.
* If Mr. X has no valid move he is caught.
* If Mr. X survives `max_rounds` rounds (default 15) he wins.

## Exhaustive Mr. X Strategy Search

`--mode solve` computes whether Mr. X has a policy that guarantees
escape against **every possible detective strategy** from the chosen
initial state.

### What is being solved

The solver checks whether there exists a Mr. X policy $\pi_X$ such that
for all detective policies $\pi_D$, Mr. X still escapes:

$$
\exists\,\pi_X\;\forall\,\pi_D:\;\text{Mr. X escapes}
$$

### How it works (high-level)

The game is treated as a finite turn-based adversarial graph search.
Each solver state contains:

- `round_number`
- `current_player`
- `mrx_position`
- `detective_positions`

For each state:

1. **Terminal checks**
  - detective on Mr. X node ⇒ loss
  - Mr. X trapped on his turn ⇒ loss
  - survived through `max_rounds` ⇒ win

2. **Expand legal moves**
  - Mr. X cannot move onto detective nodes
  - detectives cannot overlap each other

3. **Minimax-style recursion with memoization**
  - Mr. X turn = **OR node** (at least one winning child is enough)
  - detective turn = **AND node** (all detective children must still win for Mr. X)
  - memoization caches visited states to avoid recomputation

4. **Policy extraction**
  - at each Mr. X state, store a winning move if one exists,
    otherwise store a deterministic fallback move.

This is equivalent to rolling out all detective counter-strategies,
but done efficiently by state recursion + caching instead of enumerating
full detective policy tables explicitly.

### Solver output interpretation

- `Forced escape: YES` ⇒ stored Mr. X policy is guaranteed for that config.
- `Forced escape: NO` ⇒ no guaranteed escape exists; policy is best-effort.
- `states_evaluated` indicates explored state-space size.