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

    When no forced escape exists, Mr. X's policy maximises the guaranteed
    survival depth (minimax over rounds survived), so he still plays
    optimally even when eventual capture is unavoidable.
    """
    start = SolverState.from_game_state(initial_state)
    max_rounds = initial_state.max_rounds

    memo: Dict[SolverState, bool] = {}
    # depth_memo stores the guaranteed survival depth (worst-case rounds
    # Mr. X survives against optimal detectives) for each state.
    depth_memo: Dict[SolverState, int] = {}
    policy: Dict[SolverState, int] = {}
    detective_policy: Dict[SolverState, int] = {}

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

            # No forced win — pick the move that maximises guaranteed
            # survival depth so Mr. X still plays as long as possible.
            best_move = max(
                children,
                key=lambda pair: _survival_depth(board, pair[1], max_rounds, depth_memo),
            )[0]
            policy[state] = best_move
            memo[state] = False
            return False

        # Detectives are adversarial: all detective moves must still be winning
        all_children_good = True
        blocking_moves: List[int] = []
        for move, nxt in children:
            if not can_mrx_force_win(nxt):
                all_children_good = False
                blocking_moves.append(move)

        if not all_children_good and blocking_moves:
            # Detective can prevent Mr. X's escape — pick the move that
            # minimises Mr. X's guaranteed survival depth.
            best_det_move = min(
                ((m, nxt) for m, nxt in children if m in blocking_moves),
                key=lambda pair: _survival_depth(board, pair[1], max_rounds, depth_memo),
            )[0]
            detective_policy[state] = best_det_move
        else:
            # Mr. X wins regardless — detective still picks the move
            # that minimises Mr. X's survival depth.
            best_det_move = min(
                children,
                key=lambda pair: _survival_depth(board, pair[1], max_rounds, depth_memo),
            )[0]
            detective_policy[state] = best_det_move

        memo[state] = all_children_good
        return all_children_good

    forced_escape = can_mrx_force_win(start)
    return ExhaustiveResult(
        forced_escape=forced_escape,
        policy=policy,
        detective_policy=detective_policy,
        states_evaluated=len(memo),
    )


def _survival_depth(
    board: Board,
    state: SolverState,
    max_rounds: int,
    memo: Dict[SolverState, int],
) -> int:
    """Return the guaranteed number of rounds Mr. X survives from *state*.

    Uses minimax: Mr. X maximises, detectives minimise.
    A return value of ``max_rounds`` means Mr. X survives the whole game.
    """
    if state in memo:
        return memo[state]

    # Caught — survival = current round (didn't make it further)
    if state.mrx_position in state.detective_positions:
        memo[state] = state.round_number
        return state.round_number

    # Survived all rounds
    if state.round_number >= max_rounds and state.current_player == "mrx":
        memo[state] = max_rounds
        return max_rounds

    children = _next_states(board, state)

    if state.current_player == "mrx":
        if not children:
            # Mr. X is trapped
            memo[state] = state.round_number
            return state.round_number
        # Mr. X picks the move that maximises guaranteed survival
        best = max(
            _survival_depth(board, nxt, max_rounds, memo)
            for _, nxt in children
        )
        memo[state] = best
        return best
    else:
        # Detectives pick the move that minimises Mr. X's survival
        worst = min(
            _survival_depth(board, nxt, max_rounds, memo)
            for _, nxt in children
        )
        memo[state] = worst
        return worst
