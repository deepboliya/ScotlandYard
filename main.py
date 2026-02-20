"""Scotland Yard — main entry point.

Usage examples
--------------
    # Watch two random AIs play
    python main.py

    # Play as Mr. X (click to move)
    python main.py --mode play

    # Custom starting positions
    python main.py --mrx 1 --detectives 5 10

    # Text-only (no GUI)
    python main.py --no-viz
"""

from __future__ import annotations

import argparse
import sys

from game.board import create_top_right_board
from game.state import GameState
from game.engine import GameEngine
from strategies.random_strategy import RandomStrategy
from strategies.human import HumanStrategy


def _log_move(player_id: str, from_node: int, to_node: int) -> None:
    """Simple console logger for every move."""
    label = player_id.replace("_", " ").title()
    arrow = "→" if from_node != to_node else "⊘ (stuck)"
    print(f"  {label}: {from_node} {arrow} {to_node}")


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Scotland Yard — top-right board (nodes 1-20)"
    )
    parser.add_argument(
        "--mode",
        choices=["auto", "play"],
        default="auto",
        help="auto: watch AI play.  play: play as Mr. X (default: auto)",
    )
    parser.add_argument("--mrx", type=int, default=1,
                        help="Starting node for Mr. X (default: 1)")
    parser.add_argument("--detectives", type=int, nargs="+", default=[5, 10],
                        help="Starting nodes for detectives (default: 5 10)")
    parser.add_argument("--seed", type=int, default=None,
                        help="Random seed for reproducibility")
    parser.add_argument("--max-rounds", type=int, default=15,
                        help="Max rounds before Mr. X wins (default: 15)")
    parser.add_argument("--no-viz", action="store_true",
                        help="Run without visualisation (text only)")
    args = parser.parse_args()

    # ── board & validation ──────────────────────────────────────────────
    board = create_top_right_board()

    all_pos = [args.mrx] + args.detectives
    for p in all_pos:
        if p not in board:
            print(f"Error: node {p} is not on the board.  "
                  f"Valid nodes: {board.nodes}")
            sys.exit(1)
    if len(set(all_pos)) != len(all_pos):
        print("Error: all starting positions must be distinct.")
        sys.exit(1)

    state = GameState(
        mrx_position=args.mrx,
        detective_positions=list(args.detectives),
        max_rounds=args.max_rounds,
    )

    # ── text-only mode ──────────────────────────────────────────────────
    if args.no_viz:
        mrx_strat = RandomStrategy(seed=args.seed)
        det_strats = [
            RandomStrategy(seed=(args.seed or 0) + i + 1)
            for i in range(state.num_detectives)
        ]
        engine = GameEngine(board, state, mrx_strat, det_strats,
                            on_move=_log_move)

        print(f"Board: {board}")
        print(f"Mr. X starts at {state.mrx_position}  "
              f"Detectives start at {state.detective_positions}\n")

        final = engine.play_game()
        print(f"\n{final.result_str}  (round {final.round_number})")
        return

    # ── graphical modes ─────────────────────────────────────────────────
    from visualization.visualizer import GameVisualizer

    if args.mode == "play":
        # HumanStrategy for Mr. X — move_selector wired up below
        mrx_strat = HumanStrategy()
        det_strats = [
            RandomStrategy(seed=(args.seed or 0) + i + 1)
            for i in range(state.num_detectives)
        ]
        engine = GameEngine(board, state, mrx_strat, det_strats,
                            on_move=_log_move)
        viz = GameVisualizer(engine)

        # connect click-to-move
        mrx_strat.move_selector = viz.wait_for_click

        print("╔══════════════════════════════════════════╗")
        print("║   Scotland Yard — Play as Mr. X         ║")
        print("║   Click green nodes to move.             ║")
        print("║   Detectives move automatically.         ║")
        print("╚══════════════════════════════════════════╝\n")
        viz.run_interactive()

    else:
        mrx_strat = RandomStrategy(seed=args.seed)
        det_strats = [
            RandomStrategy(seed=(args.seed or 0) + i + 1)
            for i in range(state.num_detectives)
        ]
        engine = GameEngine(board, state, mrx_strat, det_strats,
                            on_move=_log_move)
        viz = GameVisualizer(engine)

        print("╔══════════════════════════════════════════╗")
        print("║   Scotland Yard — Observer Mode          ║")
        print("║   [N] Step  [R] Round  [A] Auto  [Q] Quit║")
        print("╚══════════════════════════════════════════╝\n")
        viz.run()


if __name__ == "__main__":
    main()
