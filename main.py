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
from time import perf_counter

from game.board import create_board_from_map
from game.state import GameState
from game.engine import GameEngine
from strategies.random_strategy import RandomStrategy
from strategies.human import HumanStrategy
from strategies.policy_strategy import PolicyStrategy, SerializedPolicyStrategy
from solver.exhaustive_solver import SolverState, solve_mrx_forced_escape


def _lookup_survival_depth(state: GameState, survival_depths: dict | None) -> int | None:
    """Look up survival depth exactly matching the given game state."""
    if survival_depths is None:
        return None
    key_obj = SolverState(
        round_number=state.round_number,
        current_player=state.current_player,
        mrx_position=state.mrx_position,
        detective_positions=tuple(sorted(state.detective_positions)),
    )

    # In-memory solver uses SolverState keys; loaded JSON uses string keys.
    depth = survival_depths.get(key_obj)
    if depth is not None:
        return depth

    depth = survival_depths.get(_state_to_key(key_obj))
    if depth is not None:
        return depth

    return None


def _make_move_logger(state: GameState, survival_depths: dict | None = None):
    """Return an *on_move* callback that prints round & survival depth."""

    def _log_move(player_id: str, from_node: int, to_node: int) -> None:
        label = player_id.replace("_", " ").title()
        arrow = "→" if from_node != to_node else "⊘ (stuck)"

        parts = [f"R{state.round_number:>2}"]
        depth = _lookup_survival_depth(state, survival_depths)
        if depth is not None:
            remaining = depth - state.round_number
            parts.append(f"SD={depth}(+{remaining})")
        elif survival_depths is not None:
            parts.append("SD=?")

        tag = " | ".join(parts)
        print(f"  [{tag}] {label}: {from_node} {arrow} {to_node}")

    return _log_move


def _state_to_key(state: SolverState) -> str:
    return (
        f"r={state.round_number}|p={state.current_player}|"
        f"x={state.mrx_position}|d={','.join(map(str, state.detective_positions))}"
    )


def _load_policy_bundle(path: str, board_id: str) -> tuple[
    dict[str, int],
    dict[str, list],
    dict[str, int],
    int,
    list[int],
    int,
]:
    """Load policy + board configuration from JSON.

    Returns ``(mrx_policy, detective_policy, survival_depths,
    mrx_start, detective_starts, max_rounds)``.
    """
    with open(path, "r", encoding="utf-8") as f:
        data = json.load(f)

    # New structured format (required)
    if "config" not in data or "policy" not in data:
        raise ValueError(
            "Unsupported policy JSON format. Regenerate using "
            "--mode solve --dump-policy <file>."
        )

    policy_board_id = data.get("board")
    if policy_board_id is not None and policy_board_id != board_id:
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
    survival_depths_out = data.get("survival_depths", {})

    return (
        mrx_out,
        det_out,
        survival_depths_out,
        mrx_start,
        detective_starts,
        max_rounds,
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


def _write_compact_json(f, obj: dict) -> None:
    """Write JSON matching the C++ solver format.

    Top-level and small nested objects use ``indent=2``.
    Large dicts (policy, detective_policy) put one entry per line
    with arrays kept on a single line (no per-element newlines).
    """
    f.write("{\n")
    top_keys = sorted(obj.keys())
    for ti, tk in enumerate(top_keys):
        tv = obj[tk]
        comma = ",\n" if ti < len(top_keys) - 1 else "\n"
        if type(tv) is dict and len(tv) > 20:
            # Large dict — one entry per line, inline arrays
            f.write(f"  {json.dumps(tk)}: {{\n")
            items = sorted(tv.items())
            for ii, (ik, iv) in enumerate(items):
                ic = ",\n" if ii < len(items) - 1 else "\n"
                f.write(f"    {json.dumps(ik)}: {json.dumps(iv, separators=(', ', ': '))}{ic}")
            f.write(f"  }}{comma}")
        elif type(tv) is dict:
            # Small dict — indent=2 style, inline values
            f.write(f"  {json.dumps(tk)}: {{\n")
            items = sorted(tv.items())
            for ii, (ik, iv) in enumerate(items):
                ic = ",\n" if ii < len(items) - 1 else "\n"
                f.write(f"    {json.dumps(ik)}: {json.dumps(iv, separators=(', ', ': '))}{ic}")
            f.write(f"  }}{comma}")
        else:
            f.write(f"  {json.dumps(tk)}: {json.dumps(tv, separators=(', ', ': '))}{comma}")
    f.write("}\n")


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Scotland Yard"
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
        "--dump-policy",
        action="store_true",
        help="Write solved Mr. X policy to auto-named JSON file (<map>_xX_dX_X_rX.json)",
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
    parser.add_argument(
        "--help-human",
        action="store_true",
        help="Highlight the optimal move from --policy-file in light yellow",
    )
    args = parser.parse_args()

    if args.policy_file and args.mode == "solve":
        print("Error: --policy-file cannot be used with --mode solve.")
        sys.exit(1)

    if args.help_human and not args.policy_file:
        print("Error: --help-human requires --policy-file.")
        sys.exit(1)

    map_path = f"maps/{args.map}.txt"
    board_id = map_path

    loaded_policy: dict[str, int] | None = None
    loaded_det_policy: dict[str, list] | None = None
    loaded_survival_depths: dict[str, int] | None = None
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
                loaded_det_policy,
                loaded_survival_depths,
                mrx_start,
                detective_starts,
                max_rounds,
            ) = _load_policy_bundle(args.policy_file, board_id)

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
            if loaded_survival_depths:
                print(
                    "Loaded per-state survival depths from policy file: "
                    f"{len(loaded_survival_depths)} states"
                )
        except (OSError, ValueError, json.JSONDecodeError) as exc:
            print(f"Error loading --policy-file: {exc}")
            sys.exit(1)

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

    if args.mode == "solve":
        t0 = perf_counter()
        result = solve_mrx_forced_escape(board, state)
        solve_time_s = perf_counter() - t0
        start_key = SolverState.from_game_state(state)
        guaranteed_survival_depth = result.survival_depths.get(
            start_key, state.round_number,
        )

        print("\n=== Exhaustive Adversarial Solve ===")
        print(f"Solve time: {solve_time_s:.6f} s")
        print(f"States evaluated: {result.states_evaluated}")
        print(f"Mr. X policy size: {len(result.policy)}")
        print(f"Guaranteed survival depth: {guaranteed_survival_depth}")
        print(
            f"Guaranteed survival rounds from start: "
            f"{guaranteed_survival_depth - state.round_number}"
        )
        print(
            "Forced escape:",
            "YES" if result.forced_escape else "NO",
        )

        first_move = result.policy.get(start_key)
        if first_move is not None:
            print(f"Recommended first move for Mr. X: {first_move}")

        if args.dump_policy:
            det_str = "_".join(map(str, state.detective_positions))
            map_name = args.map
            dump_path = f"{map_name}_x{state.mrx_position}_d{det_str}_r{state.max_rounds}.json"
            serialised_policy = {
                _state_to_key(k): v
                for k, v in result.policy.items()
            }
            serialised_det_policy = {
                _state_to_key(k): list(v)
                for k, v in result.detective_policy.items()
            }
            serialised_survival_depths = {
                _state_to_key(k): v
                for k, v in result.survival_depths.items()
            }
            serialised = {
                "format": "scotlandyard-policy-v2",
                "board": board_id,
                "config": {
                    "mrx_start": state.mrx_position,
                    "detective_starts": state.detective_positions,
                    "max_rounds": state.max_rounds,
                },
                "solver": {
                    "forced_escape": result.forced_escape,
                    "guaranteed_survival_depth": guaranteed_survival_depth,
                    "guaranteed_survival_rounds": (
                        guaranteed_survival_depth - state.round_number
                    ),
                    "solve_time_seconds": solve_time_s,
                    "states_evaluated": result.states_evaluated,
                    "policy_size": len(result.policy),
                    "detective_policy_size": len(result.detective_policy),
                    "survival_depths_size": len(result.survival_depths),
                },
                "policy": serialised_policy,
                "detective_policy": serialised_det_policy,
                "survival_depths": serialised_survival_depths,
            }
            with open(dump_path, "w", encoding="utf-8") as f:
                _write_compact_json(f, serialised)
            print(f"Policy written to: {dump_path}")

        return

    # ── text-only mode ──────────────────────────────────────────────────
    if args.no_viz:
        survival_depths = None

        if loaded_policy is not None:
            mrx_strat = SerializedPolicyStrategy(loaded_policy, strict=True)
            survival_depths = loaded_survival_depths
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
        log = _make_move_logger(state, survival_depths)
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
        survival_depths = None

        if loaded_policy is not None:
            mrx_strat = SerializedPolicyStrategy(loaded_policy, strict=True)
            survival_depths = loaded_survival_depths
            print("Using stored Mr. X policy from file.")
        else:
            mrx_strat = RandomStrategy(seed=args.seed)
            print("No policy file; Mr. X plays randomly.")

        det_strat = HumanStrategy()
        log = _make_move_logger(state, survival_depths)
        engine = GameEngine(board, state, mrx_strat, det_strat,
                            on_move=log)
        viz = GameVisualizer(
            engine,
            mode_label="Play as Detectives",
            mrx_policy_label=_describe_strategy(mrx_strat),
            detective_policy_label=_describe_detective_strategies(det_strat),
            survival_depths=survival_depths,
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
        survival_depths = None

        if loaded_policy is not None:
            mrx_strat = SerializedPolicyStrategy(loaded_policy, strict=True)
            survival_depths = loaded_survival_depths
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
        log = _make_move_logger(state, survival_depths)
        engine = GameEngine(board, state, mrx_strat, det_strat,
                            on_move=log)
        viz = GameVisualizer(
            engine,
            mode_label="Observer",
            mrx_policy_label=_describe_strategy(mrx_strat),
            detective_policy_label=_describe_detective_strategies(det_strat),
            survival_depths=survival_depths,
            hint_policy=hint_policy,
        )

        print("╔═══════════════════════════════════════════════════╗")
        print("║   Scotland Yard — Observer Mode                   ║")
        print("║   [N] Step [R] Round [B] Undo [A] Auto [Q] Quit   ║")
        print("╚═══════════════════════════════════════════════════╝\n")
        viz.run()


if __name__ == "__main__":
    main()
