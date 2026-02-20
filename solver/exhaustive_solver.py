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
    states_evaluated: int


def _valid_moves(
    board: Board,
    node: int,
    excluded_nodes: Iterable[int],
) -> List[int]:
    excluded = set(excluded_nodes)
    return sorted(n for n in board.neighbors(node) if n not in excluded)


def _is_terminal(
    board: Board,
    state: SolverState,
    max_rounds: int,
) -> Tuple[bool, bool]:
    """Return ``(is_terminal, mrx_wins)``."""
    # Caught immediately
    if state.mrx_position in state.detective_positions:
        return True, False

    # Mr. X survived all rounds
    if state.round_number >= max_rounds and state.current_player == "mrx":
        return True, True

    # Mr. X trapped on his turn
    if state.current_player == "mrx":
        legal = _valid_moves(
            board,
            state.mrx_position,
            state.detective_positions,
        )
        if not legal:
            return True, False

    return False, False


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
    occupied_by_other_detectives = [
        det_positions[i] for i in range(len(det_positions)) if i != idx
    ]

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
    """Compute a full-state Mr. X policy against all detective strategies.

    Returns whether Mr. X has a forced escape and a policy mapping each
    reachable Mr. X turn state to a chosen move.
    """
    start = SolverState.from_game_state(initial_state)
    max_rounds = initial_state.max_rounds

    memo: Dict[SolverState, bool] = {}
    policy: Dict[SolverState, int] = {}

    def can_mrx_force_win(state: SolverState) -> bool:
        if state in memo:
            return memo[state]

        terminal, mrx_wins = _is_terminal(board, state, max_rounds)
        if terminal:
            memo[state] = mrx_wins
            return mrx_wins

        children = _next_states(board, state)

        if state.current_player == "mrx":
            child_results: List[Tuple[int, bool]] = [
                (move, can_mrx_force_win(nxt))
                for move, nxt in children
            ]

            winning_moves = [move for move, wins in child_results if wins]
            if winning_moves:
                chosen = min(winning_moves)
                policy[state] = chosen
                memo[state] = True
                return True

            # No forced win from here; still define a deterministic fallback
            chosen = min(move for move, _ in child_results)
            policy[state] = chosen
            memo[state] = False
            return False

        # Detectives are adversarial: all detective moves must still be winning
        all_children_good = True
        for _, nxt in children:
            if not can_mrx_force_win(nxt):
                all_children_good = False
        memo[state] = all_children_good
        return all_children_good

    forced_escape = can_mrx_force_win(start)
    return ExhaustiveResult(
        forced_escape=forced_escape,
        policy=policy,
        states_evaluated=len(memo),
    )
