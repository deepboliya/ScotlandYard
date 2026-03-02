"""Exhaustive adversarial solver for Mr. X.

This module computes whether Mr. X has a **forced win** from a starting
state when detectives are fully adversarial.

Mathematically, it solves:

    ∃ strategy_MrX  such that  ∀ strategy_detectives: MrX escapes

without enumerating detective strategy functions explicitly.  Instead,
it explores every detective action branch at every detective turn.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Dict, Iterable, List, Tuple

from game.board import Board
from game.state import GameState


@dataclass(frozen=True)
class SolverState:
    """Hashable state used by the exhaustive solver."""

    round_number: int
    current_player: str
    mrx_position: int
    detective_positions: Tuple[int, ...]

    @staticmethod
    def from_game_state(state: GameState) -> "SolverState":
        return SolverState(
            round_number=state.round_number,
            current_player=state.current_player,
            mrx_position=state.mrx_position,
            detective_positions=tuple(state.detective_positions),
        )


@dataclass
class ExhaustiveResult:
    """Output of the exhaustive solver."""

    forced_escape: bool
    policy: Dict[SolverState, int]
    detective_policy: Dict[SolverState, int]
    survival_depths: Dict[SolverState, int]
    states_evaluated: int


def _valid_moves(
    board: Board,
    node: int,
    excluded_nodes: Iterable[int],
) -> List[int]:
    excluded = set(excluded_nodes)
    return sorted(n for n in board.neighbors(node) if n not in excluded)


def _next_states(board: Board, state: SolverState) -> List[Tuple[int, SolverState]]:
    """Enumerate legal transitions as ``(move, next_state)``.

    For detective turns, ``move`` is the detective's destination.
    For forced "no move" detective turns, ``move`` equals the current node.
    """
    if state.current_player == "mrx":
        legal = _valid_moves(board, state.mrx_position, state.detective_positions)
        if not legal:
            return []

        next_player = (
            "detective_0"
            if len(state.detective_positions) > 0
            else "mrx"
        )
        return [
            (
                move,
                SolverState(
                    round_number=state.round_number + 1,
                    current_player=next_player,
                    mrx_position=move,
                    detective_positions=state.detective_positions,
                ),
            )
            for move in legal
        ]

    # detective_k turn
    idx = int(state.current_player.split("_")[1])
    det_positions = list(state.detective_positions)
    from_node = det_positions[idx]
    occupied_by_other_detectives = det_positions[:idx] + det_positions[idx + 1:]

    legal = _valid_moves(board, from_node, occupied_by_other_detectives)
    if not legal:
        legal = [from_node]  # detective is stuck

    next_player = (
        f"detective_{idx + 1}"
        if idx + 1 < len(det_positions)
        else "mrx"
    )

    out: List[Tuple[int, SolverState]] = []
    for move in legal:
        nxt_det_positions = det_positions.copy()
        nxt_det_positions[idx] = move
        out.append(
            (
                move,
                SolverState(
                    round_number=state.round_number,
                    current_player=next_player,
                    mrx_position=state.mrx_position,
                    detective_positions=tuple(nxt_det_positions),
                ),
            )
        )
    return out


def solve_mrx_forced_escape(
    board: Board,
    initial_state: GameState,
) -> ExhaustiveResult:
    """Compute full-state policies for both Mr. X and detectives.

    Uses a single minimax pass over the game tree to compute the
    **guaranteed survival depth** (worst-case rounds Mr. X survives
    against optimal detectives).  A survival depth equal to
    ``max_rounds`` means Mr. X has a forced escape.

    Both ``policy`` (Mr. X) and ``detective_policy`` are populated
    during this traversal — no redundant tree walks.
    """
    start = SolverState.from_game_state(initial_state)
    max_rounds = initial_state.max_rounds

    memo: Dict[SolverState, int] = {}
    policy: Dict[SolverState, int] = {}
    detective_policy: Dict[SolverState, int] = {}

    def get_survival_depth(state: SolverState) -> int:
        """Return the guaranteed number of rounds Mr. X survives.

        Minimax: Mr. X maximises, detectives minimise.
        Also records the optimal move for each side in *policy* /
        *detective_policy*.
        """
        if state in memo:
            return memo[state]

        # ── terminal checks ─────────────────────────────────────────
        if state.mrx_position in state.detective_positions:
            memo[state] = state.round_number
            return state.round_number

        if state.round_number >= max_rounds and state.current_player == "mrx":
            memo[state] = max_rounds
            return max_rounds

        children = _next_states(board, state)

        # ── Mr. X turn (maximise) ──────────────────────────────────
        if state.current_player == "mrx":
            if not children:
                memo[state] = state.round_number
                return state.round_number

            best_depth = -1
            best_move = children[0][0]
            for move, nxt in children:
                d = get_survival_depth(nxt)
                if d > best_depth:
                    best_depth = d
                    best_move = move
                if best_depth == max_rounds:
                    break  # can't do better than surviving all rounds

            policy[state] = best_move
            memo[state] = best_depth
            return best_depth

        # ── Detective turn (minimise) ──────────────────────────────
        worst_depth = max_rounds + 1
        worst_move = children[0][0]
        for move, nxt in children:
            d = get_survival_depth(nxt)
            if d < worst_depth:
                worst_depth = d
                worst_move = move
            if worst_depth == state.round_number:
                break  # instant capture — can't do better

        detective_policy[state] = worst_move
        memo[state] = worst_depth
        return worst_depth

    guaranteed_survival = get_survival_depth(start)
    return ExhaustiveResult(
        forced_escape=(guaranteed_survival >= max_rounds),
        policy=policy,
        detective_policy=detective_policy,
        survival_depths=memo,
        states_evaluated=len(memo),
    )
