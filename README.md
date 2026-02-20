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

# Custom starting positions
python main.py --mrx 1 --detectives 5 10

# More detectives
python main.py --mrx 9 --detectives 3 6 14

# Text-only mode (no GUI)
python main.py --no-viz
```

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