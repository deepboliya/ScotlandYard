"""Scotland Yard — main entry point.

Usage examples
--------------
    # Interactive play (click to move either side, N for best move)
    python main.py --mrx-policy policy.bin --det-policy policy.bin

    # Custom starting positions
    python main.py --mrx 1 --detectives 5 10

    # Text-only (no GUI)
    python main.py --no-viz
"""

from __future__ import annotations

import argparse
import json
import struct
import sys
import time

from game.board import create_board_from_map
from game.state import GameState
from game.engine import GameEngine
from strategies.random_strategy import RandomStrategy
from strategies.human import HumanStrategy
from strategies.policy_strategy import (
    PolicyStrategy,
    SerializedPolicyStrategy,
    BinaryPolicyStrategy,
)



def _make_move_logger(state: GameState, survival_fn=None):
    """Return an *on_move* callback that prints round info."""

    def _log_move(player_id: str, from_node: int, to_node: int) -> None:
        label = player_id.replace("_", " ").title()
        arrow = "→" if from_node != to_node else "⊘ (stuck)"
        surv_str = ""
        if survival_fn is not None:
            surv = survival_fn(state)
            if surv is not None:
                surv_str = f"  [X survives: {surv}]"
        print(f"  [R{state.round_number:>2}] {label}: {from_node} {arrow} {to_node}{surv_str}")

    return _log_move


def _read_u32_le(buf: bytes, offset: int) -> int:
    return struct.unpack_from('<I', buf, offset)[0]


def _load_binary_policy_bundle(
    path: str, board_id: str | None = None
) -> tuple[
    dict[tuple, int],
    dict[tuple, list[int]],
    int,
    list[int],
    int,
    str | None,
    int,
    int | None,
    dict[tuple, int],
    dict[tuple, int],
]:
    """Load policy from compact binary .bin file.

    Returns ``(mrx_policy, detective_policy,
    mrx_start, detective_starts, max_rounds, board_path,
    num_detectives, guaranteed_survival,
    mrx_survival, det_survival)``.
    """
    t0 = time.perf_counter()
    with open(path, "rb") as f:
        raw = f.read()
    file_size = len(raw)

    if raw[:4] != b"SYP1":
        raise ValueError("Not a valid binary policy file (bad magic).")

    hdr_len = _read_u32_le(raw, 4)
    hdr_json = raw[8 : 8 + hdr_len].decode("utf-8")
    header = json.loads(hdr_json)

    policy_board_id = header.get("board")
    if board_id is not None and policy_board_id is not None and policy_board_id != board_id:
        raise ValueError(
            f"Policy board '{policy_board_id}' is incompatible with "
            f"current board '{board_id}'."
        )

    config = header["config"]
    mrx_start = config["mrx_start"]
    detective_starts = config["detective_starts"]
    max_rounds = config["max_rounds"]
    nd = config["num_detectives"]
    guaranteed_survival = config.get("guaranteed_survival")

    layout = header["binary_layout"]
    mrx_rec_len = layout["mrx_record_bytes"]   # nd + 2
    det_rec_len = layout["det_record_bytes"]   # 2*nd + 1

    offset = 8 + hdr_len

    # ── Mr. X policy ────────────────────────────────────────────
    num_mrx = _read_u32_le(raw, offset)
    offset += 4

    has_mrx_survival = (mrx_rec_len >= nd + 3)
    mrx_policy: dict[tuple, int] = {}
    mrx_survival: dict[tuple, int] = {}
    for _ in range(num_mrx):
        rec = raw[offset : offset + mrx_rec_len]
        offset += mrx_rec_len
        x = rec[0]
        dets = tuple(rec[1 : 1 + nd])
        move = rec[1 + nd]
        key = (x, *dets)
        mrx_policy[key] = move
        if has_mrx_survival:
            mrx_survival[key] = rec[2 + nd]

    # ── Detective policy ────────────────────────────────────────
    num_det = _read_u32_le(raw, offset)
    offset += 4

    has_det_survival = (det_rec_len >= 2 * nd + 2)
    det_policy: dict[tuple, list[int]] = {}
    det_survival: dict[tuple, int] = {}
    for _ in range(num_det):
        rec = raw[offset : offset + det_rec_len]
        offset += det_rec_len
        x = rec[0]
        dets = tuple(rec[1 : 1 + nd])
        moves = list(rec[1 + nd : 1 + 2 * nd])
        key = (x, *dets)
        det_policy[key] = moves
        if has_det_survival:
            det_survival[key] = rec[1 + 2 * nd]

    t1 = time.perf_counter()
    print(
        f"  Binary policy loaded: {file_size:,} bytes, "
        f"{num_mrx:,} mrx + {num_det:,} det entries "
        f"in {t1 - t0:.3f} s"
    )

    return (
        mrx_policy,
        det_policy,
        mrx_start,
        detective_starts,
        max_rounds,
        policy_board_id,
        nd,
        guaranteed_survival,
        mrx_survival,
        det_survival,
    )


def _load_json_policy_bundle(
    path: str, board_id: str | None = None
) -> tuple[
    dict[str, int],
    dict[str, list],
    int,
    list[int],
    int,
    str | None,
    int | None,
    dict[str, int],
]:
    """Load policy + board configuration from JSON.

    Returns ``(mrx_policy, detective_policy,
    mrx_start, detective_starts, max_rounds, board_path,
    guaranteed_survival, survival)``.
    """
    t0 = time.perf_counter()
    with open(path, "r", encoding="utf-8") as f:
        data = json.load(f)
    t1 = time.perf_counter()

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
    guaranteed_survival = config.get("guaranteed_survival")
    survival = data.get("survival", {})

    print(
        f"  JSON policy loaded: {len(mrx_out):,} mrx + "
        f"{len(det_out):,} det entries in {t1 - t0:.3f} s"
    )

    return (
        mrx_out,
        det_out,
        mrx_start,
        detective_starts,
        max_rounds,
        policy_board_id,
        guaranteed_survival,
        survival,
    )


def _cli_flag_present(flag: str) -> bool:
    return flag in sys.argv[1:]


def _describe_strategy(strategy) -> str:
    cls = strategy.__class__
    if cls is HumanStrategy:
        return "Human (click)"
    if cls is SerializedPolicyStrategy:
        return "Stored policy (JSON)"
    if cls is BinaryPolicyStrategy:
        return "Stored policy (binary)"
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
        "--mrx-policy",
        type=str,
        default=None,
        help=(
            "Load Mr. X policy from a .bin file (uses only Mr. X moves; "
            "overrides --mrx/--detectives/--max-rounds/--map)"
        ),
    )
    parser.add_argument(
        "--det-policy",
        type=str,
        default=None,
        help=(
            "Load detective policy from a .bin file (uses only detective moves; "
            "overrides --mrx/--detectives/--max-rounds/--map)"
        ),
    )
    args = parser.parse_args()

    # Both policy files are required
    if not args.mrx_policy or not args.det_policy:
        print("Error: Both --mrx-policy and --det-policy are required.")
        sys.exit(1)

    loaded_policy = None
    loaded_det_policy = None
    is_binary_policy = False
    guaranteed_survival: int | None = None
    survival_fn = None
    mrx_start = args.mrx
    detective_starts = list(args.detectives)
    max_rounds = args.max_rounds

    # Determine map_path: use policy file's board if policy given,
    # otherwise fall back to --map CLI argument.
    has_policy = args.mrx_policy or args.det_policy
    if has_policy:
        try:
            # If --map was explicitly passed, validate against the policy
            explicit_map = _cli_flag_present("--map")
            cli_board_id = f"maps/{args.map}.txt" if explicit_map else None

            cli_mrx = args.mrx
            cli_detectives = list(args.detectives)
            cli_max_rounds = args.max_rounds

            # ── Load Mr. X policy file ─────────────────────────────────
            _mrx_survival: dict = {}
            _det_survival_from_mrx: dict = {}
            policy_board_path = None
            pol_mrx_start = None
            pol_det_starts = None
            pol_max_rounds = None
            _nd = None

            if args.mrx_policy:
                if not args.mrx_policy.endswith(".bin"):
                    raise ValueError("--mrx-policy only supports .bin files.")
                (
                    loaded_policy,
                    _unused_det,
                    pol_mrx_start,
                    pol_det_starts,
                    pol_max_rounds,
                    policy_board_path,
                    _nd,
                    guaranteed_survival,
                    _mrx_survival,
                    _det_survival_from_mrx,
                ) = _load_binary_policy_bundle(args.mrx_policy, cli_board_id)
                is_binary_policy = True
                print(f"Loaded Mr. X policy file: {args.mrx_policy}")

            # ── Load detective policy file ─────────────────────────────
            _det_survival: dict = {}

            if args.det_policy:
                if not args.det_policy.endswith(".bin"):
                    raise ValueError("--det-policy only supports .bin files.")
                (
                    _unused_mrx,
                    loaded_det_policy,
                    det_pol_mrx_start,
                    det_pol_det_starts,
                    det_pol_max_rounds,
                    det_policy_board_path,
                    det_nd,
                    det_guaranteed_survival,
                    _mrx_survival_from_det,
                    _det_survival,
                ) = _load_binary_policy_bundle(args.det_policy, cli_board_id)
                is_binary_policy = True
                print(f"Loaded detective policy file: {args.det_policy}")

                # If no mrx-policy was given, use config from det-policy
                if not args.mrx_policy:
                    pol_mrx_start = det_pol_mrx_start
                    pol_det_starts = det_pol_det_starts
                    pol_max_rounds = det_pol_max_rounds
                    policy_board_path = det_policy_board_path
                    _nd = det_nd
                    guaranteed_survival = det_guaranteed_survival

            if pol_mrx_start is not None:
                mrx_start = pol_mrx_start
            if pol_det_starts is not None:
                detective_starts = pol_det_starts
            if pol_max_rounds is not None:
                max_rounds = pol_max_rounds

            if policy_board_path is not None:
                map_path = policy_board_path
            else:
                map_path = f"maps/{args.map}.txt"

            board_id = map_path

            mismatches: list[str] = []
            if _cli_flag_present("--mrx") and pol_mrx_start is not None and cli_mrx != pol_mrx_start:
                mismatches.append(
                    f"--mrx={cli_mrx} (policy has {pol_mrx_start})"
                )
            if _cli_flag_present("--detectives") and pol_det_starts is not None and cli_detectives != pol_det_starts:
                mismatches.append(
                    f"--detectives={cli_detectives} (policy has {pol_det_starts})"
                )
            elif pol_det_starts is None and is_binary_policy and _nd is not None and len(cli_detectives) != _nd:
                mismatches.append(
                    f"policy requires exactly {_nd} detectives, but got {len(cli_detectives)} (use --detectives)"
                )
            # Allow --max-rounds to be <= policy's max_rounds (optimal for N rounds is also optimal for <N)
            if _cli_flag_present("--max-rounds") and pol_max_rounds is not None and cli_max_rounds > pol_max_rounds:
                mismatches.append(
                    f"--max-rounds={cli_max_rounds} exceeds policy max {pol_max_rounds}"
                )
            if mismatches:
                raise ValueError(
                    "Passed arguments do not match policy config: "
                    + "; ".join(mismatches)
                )

            # Use CLI max-rounds if specified and valid, otherwise use policy's
            if _cli_flag_present("--max-rounds") and cli_max_rounds <= pol_max_rounds:
                max_rounds = cli_max_rounds

            # Build per-state survival lookup
            # mrx_survival comes from --mrx-policy, det_survival from --det-policy
            _ms = _mrx_survival
            _ds = _det_survival
            if _ms or _ds:
                def _surv_fn(st, ms=_ms, ds=_ds):
                    k = (st.mrx_position, *st.detective_positions)
                    return ms.get(k) if st.current_player == "mrx" else ds.get(k)
                survival_fn = _surv_fn

            surv_str = (
                f", guaranteed_survival={guaranteed_survival}/{max_rounds}"
                if guaranteed_survival is not None else ""
            )
            print(
                "Using configuration from policy: "
                f"map={map_path}, mrx={mrx_start}, detectives={detective_starts}, "
                f"max_rounds={max_rounds}{surv_str}"
            )
        except (OSError, ValueError, json.JSONDecodeError) as exc:
            print(f"Error loading policy file: {exc}")
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
            if is_binary_policy:
                mrx_strat = BinaryPolicyStrategy(loaded_policy, strict=True)
            else:
                mrx_strat = SerializedPolicyStrategy(loaded_policy, strict=True)
            print("Using stored Mr. X policy from file.")
        else:
            mrx_strat = RandomStrategy(seed=args.seed)
            print("No policy file; Mr. X plays randomly.")

        if loaded_det_policy:
            if is_binary_policy:
                det_strat = BinaryPolicyStrategy(
                    {},
                    binary_det_policy=loaded_det_policy,
                    strict=False,
                )
            else:
                det_strat = SerializedPolicyStrategy(
                    {},  # Mr. X policy not used here
                    serialized_det_policy=loaded_det_policy,
                    strict=False,
                )
        else:
            det_strat = RandomStrategy(seed=(args.seed or 0) + 1)
        log = _make_move_logger(state, survival_fn)
        engine = GameEngine(board, state, mrx_strat, det_strat,
                            on_move=log)

        print(f"Board: {board}")
        print(f"Mr. X starts at {state.mrx_position}  "
              f"Detectives start at {state.detective_positions}\n")

        final = engine.play_game()
        print(f"\n{final.result_str}  (round {final.round_number})")
        return

    # ── graphical interactive mode ─────────────────────────────────────
    from visualization.visualizer import GameVisualizer

    # Use HumanStrategy for both Mr. X and detectives
    mrx_strat = HumanStrategy()
    det_strat = HumanStrategy()

    log = _make_move_logger(state, survival_fn)
    engine = GameEngine(board, state, mrx_strat, det_strat, on_move=log)
    viz = GameVisualizer(
        engine,
        mode_label="Interactive",
        mrx_policy_label=_describe_strategy(mrx_strat),
        detective_policy_label=_describe_detective_strategies(det_strat),
        guaranteed_survival=guaranteed_survival,
        survival_fn=survival_fn,
        mrx_policy=loaded_policy,
        det_policy=loaded_det_policy,
    )

    # Connect click-to-move for both strategies
    mrx_strat.move_selector = viz.wait_for_click
    det_strat.move_selector = viz.wait_for_click

    print("╔═══════════════════════════════════════════════════════╗")
    print("║   Scotland Yard — Interactive Mode                    ║")
    print("║   Click green nodes to move for either side.          ║")
    print("║   [N] Play best move  [Q] Quit                        ║")
    print("╚═══════════════════════════════════════════════════════╝\n")
    viz.run_interactive()


if __name__ == "__main__":
    main()
