"""Scotland Yard — main entry point.

Usage examples
--------------
    # Watch two random AIs play
    python main.py

    # Play as Mr. X (click to move)
    python main.py --mode play-mrx

    # Custom starting positions
    python main.py --mrx 1 --detectives 5 10

    # Text-only (no GUI)
    python main.py --no-viz
"""

from __future__ import annotations

import argparse
import json
import sys
from typing import Any

from game.board import create_top_right_board
from game.state import GameState
from game.engine import GameEngine
from strategies.random_strategy import RandomStrategy
from strategies.human import HumanStrategy
from strategies.policy_strategy import PolicyStrategy, SerializedPolicyStrategy
from solver.exhaustive_solver import SolverState, solve_mrx_forced_escape


def _log_move(player_id: str, from_node: int, to_node: int) -> None:
    """Simple console logger for every move."""
    label = player_id.replace("_", " ").title()
    arrow = "→" if from_node != to_node else "⊘ (stuck)"
    print(f"  {label}: {from_node} {arrow} {to_node}")


def _state_to_key(state: SolverState) -> str:
    return (
        f"r={state.round_number}|p={state.current_player}|"
        f"x={state.mrx_position}|d={','.join(map(str, state.detective_positions))}"
    )


def _load_policy_bundle(path: str) -> tuple[dict[str, int], int, list[int], int]:
    """Load policy + board configuration from JSON.

    Returns ``(policy, mrx_start, detective_starts, max_rounds)``.
    """
    with open(path, "r", encoding="utf-8") as f:
        data = json.load(f)

    if not isinstance(data, dict):
        raise ValueError("Policy JSON must be a JSON object.")

    # New structured format (required)
    if "config" not in data or "policy" not in data:
        raise ValueError(
            "Unsupported policy JSON format. Regenerate using "
            "--mode solve --dump-policy <file>."
        )

    config: Any = data["config"]
    policy_obj: Any = data["policy"]

    if not isinstance(config, dict):
        raise ValueError("Policy JSON field 'config' must be an object.")
    if not isinstance(policy_obj, dict):
        raise ValueError("Policy JSON field 'policy' must be an object.")

    mrx_start = config.get("mrx_start")
    detective_starts = config.get("detective_starts")
    max_rounds = config.get("max_rounds")

    if not isinstance(mrx_start, int):
        raise ValueError("config.mrx_start must be an integer.")
    if not isinstance(detective_starts, list) or not all(
        isinstance(x, int) for x in detective_starts
    ):
        raise ValueError("config.detective_starts must be a list of integers.")
    if not isinstance(max_rounds, int):
        raise ValueError("config.max_rounds must be an integer.")

    out: dict[str, int] = {}
    for k, v in policy_obj.items():
        if not isinstance(k, str):
            continue
        if isinstance(v, int):
            out[k] = v
    if not out:
        raise ValueError("Policy JSON has no valid entries.")
    return out, mrx_start, detective_starts, max_rounds


def _cli_flag_present(flag: str) -> bool:
    return flag in sys.argv[1:]


def _describe_strategy(strategy) -> str:
    if isinstance(strategy, HumanStrategy):
        return "Human (click)"
    if isinstance(strategy, SerializedPolicyStrategy):
        return "Stored policy (JSON)"
    if isinstance(strategy, PolicyStrategy):
        return "Solved policy"
    if isinstance(strategy, RandomStrategy):
        return "Random"
    return strategy.__class__.__name__


def _describe_detective_strategies(det_strats: list) -> str:
    if not det_strats:
        return "None"
    labels = [_describe_strategy(s) for s in det_strats]
    if all(lbl == labels[0] for lbl in labels):
        return f"{labels[0]} ×{len(labels)}"
    return "; ".join(f"D{i}: {lbl}" for i, lbl in enumerate(labels))


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Scotland Yard — top-right board (nodes 1-20)"
    )
    parser.add_argument(
        "--mode",
        choices=["auto", "play-mrx", "play-detective", "solve"],
        default="auto",
        help=(
            "auto: watch AI play. play-mrx: play as Mr. X. "
            "play-detective: play as detectives. "
            "solve: exhaustive adversarial solve"
        ),
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
    parser.add_argument(
        "--dump-policy",
        type=str,
        default=None,
        help="Optional JSON file to write solved Mr. X state->move policy",
    )
    parser.add_argument(
        "--policy-file",
        type=str,
        default=None,
        help=(
            "Load Mr. X policy+configuration from dumped JSON file "
            "(overrides --mrx/--detectives/--max-rounds)"
        ),
    )
    args = parser.parse_args()

    if args.policy_file and args.mode == "solve":
        print("Error: --policy-file cannot be used with --mode solve.")
        sys.exit(1)
    if args.policy_file and args.mode == "play-mrx":
        print("Error: --policy-file cannot be used with --mode play-mrx (you are Mr. X).")
        sys.exit(1)

    loaded_policy: dict[str, int] | None = None
    mrx_start = args.mrx
    detective_starts = list(args.detectives)
    max_rounds = args.max_rounds

    if args.policy_file:
        try:
            cli_mrx = args.mrx
            cli_detectives = list(args.detectives)
            cli_max_rounds = args.max_rounds

            (
                loaded_policy,
                mrx_start,
                detective_starts,
                max_rounds,
            ) = _load_policy_bundle(args.policy_file)

            mismatches: list[str] = []
            if _cli_flag_present("--mrx") and cli_mrx != mrx_start:
                mismatches.append(
                    f"--mrx={cli_mrx} (policy has {mrx_start})"
                )
            if _cli_flag_present("--detectives") and cli_detectives != detective_starts:
                mismatches.append(
                    f"--detectives={cli_detectives} (policy has {detective_starts})"
                )
            if _cli_flag_present("--max-rounds") and cli_max_rounds != max_rounds:
                mismatches.append(
                    f"--max-rounds={cli_max_rounds} (policy has {max_rounds})"
                )
            if mismatches:
                raise ValueError(
                    "Passed arguments do not match policy config: "
                    + "; ".join(mismatches)
                )

            print(f"Loaded policy file: {args.policy_file}")
            print(
                "Using configuration from policy file: "
                f"mrx={mrx_start}, detectives={detective_starts}, "
                f"max_rounds={max_rounds}"
            )
        except (OSError, ValueError, json.JSONDecodeError) as exc:
            print(f"Error loading --policy-file: {exc}")
            sys.exit(1)

    # ── board & validation ──────────────────────────────────────────────
    board = create_top_right_board()

    all_pos = [mrx_start] + detective_starts
    for p in all_pos:
        if p not in board:
            print(f"Error: node {p} is not on the board.  "
                  f"Valid nodes: {board.nodes}")
            sys.exit(1)
    if len(set(all_pos)) != len(all_pos):
        print("Error: all starting positions must be distinct.")
        sys.exit(1)

    state = GameState(
        mrx_position=mrx_start,
        detective_positions=detective_starts,
        max_rounds=max_rounds,
    )

    if args.mode == "solve":
        result = solve_mrx_forced_escape(board, state)
        print("\n=== Exhaustive Adversarial Solve ===")
        print(f"States evaluated: {result.states_evaluated}")
        print(f"Mr. X policy size: {len(result.policy)}")
        print(
            "Forced escape:",
            "YES" if result.forced_escape else "NO",
        )

        start_key = SolverState.from_game_state(state)
        first_move = result.policy.get(start_key)
        if first_move is not None:
            print(f"Recommended first move for Mr. X: {first_move}")

        if args.dump_policy:
            serialised_policy = {
                _state_to_key(k): v
                for k, v in result.policy.items()
            }
            serialised = {
                "format": "scotlandyard-policy-v2",
                "board": "top-right-simple-v1",
                "config": {
                    "mrx_start": state.mrx_position,
                    "detective_starts": state.detective_positions,
                    "max_rounds": state.max_rounds,
                },
                "solver": {
                    "forced_escape": result.forced_escape,
                    "states_evaluated": result.states_evaluated,
                    "policy_size": len(result.policy),
                },
                "policy": serialised_policy,
            }
            with open(args.dump_policy, "w", encoding="utf-8") as f:
                json.dump(serialised, f, indent=2, sort_keys=True)
            print(f"Policy written to: {args.dump_policy}")

        return

    # ── text-only mode ──────────────────────────────────────────────────
    if args.no_viz:
        if loaded_policy is not None:
            mrx_strat = SerializedPolicyStrategy(loaded_policy)
        else:
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

    if args.mode == "play-mrx":
        # HumanStrategy for Mr. X — move_selector wired up below
        mrx_strat = HumanStrategy()
        det_strats = [
            RandomStrategy(seed=(args.seed or 0) + i + 1)
            for i in range(state.num_detectives)
        ]
        engine = GameEngine(board, state, mrx_strat, det_strats,
                            on_move=_log_move)
        viz = GameVisualizer(
            engine,
            mode_label="Play as Mr. X",
            mrx_policy_label=_describe_strategy(mrx_strat),
            detective_policy_label=_describe_detective_strategies(det_strats),
        )

        # connect click-to-move
        mrx_strat.move_selector = viz.wait_for_click

        print("╔══════════════════════════════════════════╗")
        print("║   Scotland Yard — Play as Mr. X         ║")
        print("║   Click green nodes to move.             ║")
        print("║   Detectives move automatically.         ║")
        print("╚══════════════════════════════════════════╝\n")
        viz.run_interactive()

    elif args.mode == "play-detective":
        if loaded_policy is not None:
            mrx_strat = SerializedPolicyStrategy(loaded_policy)
            print("Using stored Mr. X policy from file.")
        else:
            solve = solve_mrx_forced_escape(board, state)
            if solve.forced_escape:
                print("Using solved policy strategy for Mr. X (forced escape exists).")
                mrx_strat = PolicyStrategy(solve.policy)
            else:
                print("No forced escape policy found; using random Mr. X strategy.")
                mrx_strat = RandomStrategy(seed=args.seed)

        det_strats = [HumanStrategy() for _ in range(state.num_detectives)]
        engine = GameEngine(board, state, mrx_strat, det_strats,
                            on_move=_log_move)
        viz = GameVisualizer(
            engine,
            mode_label="Play as Detectives",
            mrx_policy_label=_describe_strategy(mrx_strat),
            detective_policy_label=_describe_detective_strategies(det_strats),
        )

        for strat in det_strats:
            strat.move_selector = viz.wait_for_click

        print("╔══════════════════════════════════════════╗")
        print("║ Scotland Yard — Play as Detectives       ║")
        print("║ Mr. X uses solved policy when available. ║")
        print("║ Click green nodes for each detective.    ║")
        print("╚══════════════════════════════════════════╝\n")
        viz.run_interactive()

    else:
        if loaded_policy is not None:
            mrx_strat = SerializedPolicyStrategy(loaded_policy)
            print("Using stored Mr. X policy from file.")
        else:
            solve = solve_mrx_forced_escape(board, state)
            if solve.forced_escape:
                print("Using solved policy strategy for Mr. X (forced escape exists).")
                mrx_strat = PolicyStrategy(solve.policy)
            else:
                print("No forced escape policy found; using random Mr. X strategy.")
                mrx_strat = RandomStrategy(seed=args.seed)

        det_strats = [
            RandomStrategy(seed=(args.seed or 0) + i + 1)
            for i in range(state.num_detectives)
        ]
        engine = GameEngine(board, state, mrx_strat, det_strats,
                            on_move=_log_move)
        viz = GameVisualizer(
            engine,
            mode_label="Observer",
            mrx_policy_label=_describe_strategy(mrx_strat),
            detective_policy_label=_describe_detective_strategies(det_strats),
        )

        print("╔══════════════════════════════════════════╗")
        print("║   Scotland Yard — Observer Mode          ║")
        print("║   [N] Step  [R] Round  [A] Auto  [Q] Quit║")
        print("╚══════════════════════════════════════════╝\n")
        viz.run()


if __name__ == "__main__":
    main()
