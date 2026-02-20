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

## Quick Start

```bash
pip install -r requirements.txt

# Watch random AIs play (press N/R/A/Q)
python main.py

# Play as Mr. X (click green nodes to move)
python main.py --mode play

# Play as detectives against Mr. X policy
python main.py --mode play-detective

# Play as detectives against a stored dumped policy
python main.py --mode play-detective --policy-file x_1_d_5_10.json

# Custom starting positions
python main.py --mrx 1 --detectives 5 10

# More detectives
python main.py --mrx 9 --detectives 3 6 14

# Text-only mode (no GUI)
python main.py --no-viz

# Exhaustive adversarial solve (all detective strategies)
python main.py --mode solve

# Save the full Mr. X state->move policy map
python main.py --mode solve --dump-policy policy.json

# Play from stored policy; configuration is read from JSON
# (--mrx/--detectives/--max-rounds are ignored)
python main.py --mode play-detective --policy-file policy.json
```

## Policy JSON Format

Dumped policy files now include both the policy and the exact board
configuration they were solved for:

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

## Controls

| Key | Action |
|-----|--------|
| **N** | Step — one player's move |
| **R** | Round — Mr. X + all detectives |
| **A** | Auto-play to completion |
| **Q** | Quit |

In **play** mode, click on green-highlighted nodes to move Mr. X.

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

Use `--mode solve` to compute whether Mr. X has a policy that guarantees
escape against **every possible detective strategy**.

This is solved as a turn-based adversarial game:

* On Mr. X turns: existential choice (`any` move can be chosen).
* On detective turns: universal choice (`all` detective moves must still
  allow Mr. X to escape).

So the solver checks:

$$
\exists\,\pi_{X}\;\forall\,\pi_{D}:\;\text{Mr. X escapes}
$$

without explicitly enumerating huge detective policy tables.