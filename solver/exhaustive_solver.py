"""Exhaustive adversarial solver for Mr. X.

This module computes whether Mr. X has a **forced win** from a starting
state when detectives are fully adversarial.

Mathematically, it solves:

    ∃ strategy_MrX  such that  ∀ strategy_detectives: MrX escapes

Detectives move as a group: after Mr. X moves, all detectives choose
their destinations simultaneously.  The solver enumerates every valid
joint detective placement to find the detective-optimal response.
"""

from __future__ import annotations

from dataclasses import dataclass
from itertools import product
from typing import Dict, Iterable, List, Tuple

from game.board import Board
from game.state import GameState


@dataclass(frozen=True)
class SolverState:
    """Hashable state used by the exhaustive solver.

    ``detective_positions`` is always stored sorted so that
    permutations of the same set of positions are treated as
    the same state (detectives are interchangeable).
    """

    round_number: int
    current_player: str          # "mrx" or "detectives"
    mrx_position: int
    detective_positions: Tuple[int, ...]

    def __post_init__(self):
        # Sort so that e.g. (10, 5) and (5, 10) hash identically.
        object.__setattr__(
            self, 'detective_positions',
            tuple(sorted(self.detective_positions)),
        )

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
    detective_policy: Dict[SolverState, Tuple[int, ...]]
    survival_depths: Dict[SolverState, int]
    states_evaluated: int


def _valid_moves(
    board: Board,
    node: int,
    excluded_nodes: Iterable[int],
) -> List[int]:
    excluded = set(excluded_nodes)
    return sorted(n for n in board.neighbors(node) if n not in excluded)


def _mrx_next_states(
    board: Board, state: SolverState
) -> List[Tuple[int, SolverState]]:
    """Enumerate Mr. X's legal moves as ``(move, next_state)``."""
    legal = _valid_moves(board, state.mrx_position, state.detective_positions)
    if not legal:
        return []

    next_player = "detectives" if len(state.detective_positions) > 0 else "mrx"
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


def _detective_next_states(
    board: Board, state: SolverState
) -> List[Tuple[Tuple[int, ...], SolverState]]:
    """Enumerate all valid joint detective placements.

    Returns ``(new_positions_tuple, next_state)`` pairs where every
    detective has moved to a valid neighbour (or stayed put if stuck).
    Detectives may not share nodes with each other.
    """
    det_positions = list(state.detective_positions)
    num_dets = len(det_positions)

    # For each detective, compute their individual legal moves
    # (excluding other detectives' *current* positions).
    per_det_moves: List[List[int]] = []
    for idx in range(num_dets):
        others = det_positions[:idx] + det_positions[idx + 1:]
        moves = _valid_moves(board, det_positions[idx], others)
        if not moves:
            moves = [det_positions[idx]]  # stuck — stay put
        per_det_moves.append(moves)

    # Enumerate all joint placements, filtering out collisions
    # and deduplicating permutations (detectives are interchangeable).
    seen: set[Tuple[int, ...]] = set()
    results: List[Tuple[Tuple[int, ...], SolverState]] = []
    for combo in product(*per_det_moves):
        # No two detectives may occupy the same node.
        if len(set(combo)) != num_dets:
            continue
        sorted_pos = tuple(sorted(combo))
        if sorted_pos in seen:
            continue
        seen.add(sorted_pos)
        # Store original *combo* (indexed by sorted current positions)
        # so the policy records which move each detective should make.
        # SolverState stores the sorted target positions for hashing.
        results.append(
            (
                combo,
                SolverState(
                    round_number=state.round_number,
                    current_player="mrx",
                    mrx_position=state.mrx_position,
                    detective_positions=sorted_pos,
                ),
            )
        )
    return results


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
    detective_policy: Dict[SolverState, Tuple[int, ...]] = {}

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

        # ── Mr. X turn (maximise) ──────────────────────────────────
        if state.current_player == "mrx":
            children = _mrx_next_states(board, state)
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
        children = _detective_next_states(board, state)
        if not children:
            # No valid joint placement (shouldn't happen normally)
            memo[state] = max_rounds
            return max_rounds

        worst_depth = max_rounds + 1
        worst_combo = children[0][0]
        for combo, nxt in children:
            d = get_survival_depth(nxt)
            if d < worst_depth:
                worst_depth = d
                worst_combo = combo
            if worst_depth == state.round_number:
                break  # instant capture — can't do better

        detective_policy[state] = worst_combo
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
