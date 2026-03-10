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

from game.board import create_board_from_map
from game.state import GameState
from game.engine import GameEngine
from strategies.random_strategy import RandomStrategy
from strategies.human import HumanStrategy
from strategies.policy_strategy import PolicyStrategy, SerializedPolicyStrategy



def _make_move_logger(state: GameState):
    """Return an *on_move* callback that prints round info."""

    def _log_move(player_id: str, from_node: int, to_node: int) -> None:
        label = player_id.replace("_", " ").title()
        arrow = "→" if from_node != to_node else "⊘ (stuck)"
        print(f"  [R{state.round_number:>2}] {label}: {from_node} {arrow} {to_node}")

    return _log_move


def _load_policy_bundle(path: str, board_id: str | None = None) -> tuple[
    dict[str, int],
    dict[str, list],
    int,
    list[int],
    int,
    str | None,
]:
    """Load policy + board configuration from JSON.

    Returns ``(mrx_policy, detective_policy,
    mrx_start, detective_starts, max_rounds, board_path)``.
    """
    with open(path, "r", encoding="utf-8") as f:
        data = json.load(f)

    # New structured format (required)
    if "config" not in data or "policy" not in data:
        raise ValueError(
            "Unsupported policy JSON format. Regenerate using "
            "the C++ solver."
        )

    policy_board_id = data.get("board")
    if board_id is not None and policy_board_id is not None and policy_board_id != board_id:
        raise ValueError(
            f"Policy board '{policy_board_id}' is incompatible with current board '{board_id}'."
        )

    config = data["config"]
    mrx_start = config["mrx_start"]
    detective_starts = config["detective_starts"]
    max_rounds = config["max_rounds"]

    mrx_out = data["policy"]
    if not mrx_out:
        raise ValueError("Policy JSON has no valid Mr. X policy entries.")

    det_out = data.get("detective_policy", {})

    return (
        mrx_out,
        det_out,
        mrx_start,
        detective_starts,
        max_rounds,
        policy_board_id,
    )


def _cli_flag_present(flag: str) -> bool:
    return flag in sys.argv[1:]


def _describe_strategy(strategy) -> str:
    cls = strategy.__class__
    if cls is HumanStrategy:
        return "Human (click)"
    if cls is SerializedPolicyStrategy:
        return "Stored policy (JSON)"
    if cls is PolicyStrategy:
        return "Solved policy"
    if cls is RandomStrategy:
        return "Random"
    return strategy.__class__.__name__


def _describe_detective_strategies(det_strat) -> str:
    if det_strat is None:
        return "None"
    return _describe_strategy(det_strat)


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Scotland Yard"
    )
    parser.add_argument(
        "--mode",
        choices=["auto", "play-mrx", "play-detective"],
        default="auto",
        help=(
            "auto: watch AI play. play-mrx: play as Mr. X. "
            "play-detective: play as detectives."
        ),
    )
    parser.add_argument("--map", type=str, default="first50",
                        help="Map name in maps/ directory, without .txt (default: first50)")
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
        "--policy-file",
        type=str,
        default=None,
        help=(
            "Load Mr. X policy+configuration from dumped JSON file "
            "(overrides --mrx/--detectives/--max-rounds)"
        ),
    )
    parser.add_argument(
        "--help-human",
        action="store_true",
        help="Highlight the optimal move from --policy-file in light yellow",
    )
    args = parser.parse_args()

    if args.help_human and not args.policy_file:
        print("Error: --help-human requires --policy-file.")
        sys.exit(1)

    loaded_policy: dict[str, int] | None = None
    loaded_det_policy: dict[str, list] | None = None
    mrx_start = args.mrx
    detective_starts = list(args.detectives)
    max_rounds = args.max_rounds

    # Determine map_path: use policy file's board if --policy-file given,
    # otherwise fall back to --map CLI argument.
    if args.policy_file:
        try:
            # If --map was explicitly passed, validate against the policy
            explicit_map = _cli_flag_present("--map")
            cli_board_id = f"maps/{args.map}.txt" if explicit_map else None

            cli_mrx = args.mrx
            cli_detectives = list(args.detectives)
            cli_max_rounds = args.max_rounds

            (
                loaded_policy,
                loaded_det_policy,
                mrx_start,
                detective_starts,
                max_rounds,
                policy_board_path,
            ) = _load_policy_bundle(args.policy_file, cli_board_id)

            if policy_board_path is not None:
                map_path = policy_board_path
            elif explicit_map:
                map_path = f"maps/{args.map}.txt"
            else:
                map_path = f"maps/{args.map}.txt"

            board_id = map_path

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
                f"map={map_path}, mrx={mrx_start}, detectives={detective_starts}, "
                f"max_rounds={max_rounds}"
            )
        except (OSError, ValueError, json.JSONDecodeError) as exc:
            print(f"Error loading --policy-file: {exc}")
            sys.exit(1)
    else:
        map_path = f"maps/{args.map}.txt"
        board_id = map_path

    # ── board & validation ──────────────────────────────────────────────
    board = create_board_from_map(map_path)

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

    # ── text-only mode ──────────────────────────────────────────────────
    if args.no_viz:
        if loaded_policy is not None:
            mrx_strat = SerializedPolicyStrategy(loaded_policy, strict=True)
            print("Using stored Mr. X policy from file.")
        else:
            mrx_strat = RandomStrategy(seed=args.seed)
            print("No policy file; Mr. X plays randomly.")

        if loaded_det_policy:
            det_strat = SerializedPolicyStrategy(
                {},  # Mr. X policy not used here
                serialized_det_policy=loaded_det_policy,
                strict=False,
            )
        else:
            det_strat = RandomStrategy(seed=(args.seed or 0) + 1)
        log = _make_move_logger(state)
        engine = GameEngine(board, state, mrx_strat, det_strat,
                            on_move=log)

        print(f"Board: {board}")
        print(f"Mr. X starts at {state.mrx_position}  "
              f"Detectives start at {state.detective_positions}\n")

        final = engine.play_game()
        print(f"\n{final.result_str}  (round {final.round_number})")
        return

    # ── graphical modes ─────────────────────────────────────────────────
    from visualization.visualizer import GameVisualizer

    # Build serialized hint policy from loaded file when --help-human
    hint_policy: dict | None = None
    if args.help_human and loaded_policy is not None:
        hint_policy = dict(loaded_policy)
        if loaded_det_policy:
            hint_policy.update(loaded_det_policy)

    if args.mode == "play-mrx":
        # HumanStrategy for Mr. X — move_selector wired up below
        mrx_strat = HumanStrategy()
        if loaded_det_policy:
            det_strat = SerializedPolicyStrategy(
                {},
                serialized_det_policy=loaded_det_policy,
                strict=False,
            )
        else:
            det_strat = RandomStrategy(seed=(args.seed or 0) + 1)
        log = _make_move_logger(state)
        engine = GameEngine(board, state, mrx_strat, det_strat,
                            on_move=log)
        viz = GameVisualizer(
            engine,
            mode_label="Play as Mr. X",
            mrx_policy_label=_describe_strategy(mrx_strat),
            detective_policy_label=_describe_detective_strategies(det_strat),
            hint_policy=hint_policy,
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
            mrx_strat = SerializedPolicyStrategy(loaded_policy, strict=True)
            print("Using stored Mr. X policy from file.")
        else:
            mrx_strat = RandomStrategy(seed=args.seed)
            print("No policy file; Mr. X plays randomly.")

        det_strat = HumanStrategy()
        log = _make_move_logger(state)
        engine = GameEngine(board, state, mrx_strat, det_strat,
                            on_move=log)
        viz = GameVisualizer(
            engine,
            mode_label="Play as Detectives",
            mrx_policy_label=_describe_strategy(mrx_strat),
            detective_policy_label=_describe_detective_strategies(det_strat),
            hint_policy=hint_policy,
        )

        det_strat.move_selector = viz.wait_for_click

        print("╔══════════════════════════════════════════╗")
        print("║ Scotland Yard — Play as Detectives       ║")
        print("║ Mr. X uses policy file when available.   ║")
        print("║ Click green nodes for each detective.    ║")
        print("╚══════════════════════════════════════════╝\n")
        viz.run_interactive()

    else:
        if loaded_policy is not None:
            mrx_strat = SerializedPolicyStrategy(loaded_policy, strict=True)
            print("Using stored Mr. X policy from file.")
        else:
            mrx_strat = RandomStrategy(seed=args.seed)
            print("No policy file; Mr. X plays randomly.")

        if loaded_det_policy:
            det_strat = SerializedPolicyStrategy(
                {},
                serialized_det_policy=loaded_det_policy,
                strict=False,
            )
        else:
            det_strat = RandomStrategy(seed=(args.seed or 0) + 1)
        log = _make_move_logger(state)
        engine = GameEngine(board, state, mrx_strat, det_strat,
                            on_move=log)
        viz = GameVisualizer(
            engine,
            mode_label="Observer",
            mrx_policy_label=_describe_strategy(mrx_strat),
            detective_policy_label=_describe_detective_strategies(det_strat),
            hint_policy=hint_policy,
        )

        print("╔═══════════════════════════════════════════════════╗")
        print("║   Scotland Yard — Observer Mode                   ║")
        print("║   [N] Step [R] Round [B] Undo [A] Auto [Q] Quit   ║")
        print("╚═══════════════════════════════════════════════════╝\n")
        viz.run()


if __name__ == "__main__":
    main()
