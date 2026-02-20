"""Strategy that plays from a precomputed full-state policy map."""

from __future__ import annotations

from typing import Dict, List

from game.board import Board
from game.state import GameState
from solver.exhaustive_solver import SolverState
from strategies.base import Strategy


class PolicyStrategy(Strategy):
    """Mr. X strategy backed by a state -> move mapping."""

    def __init__(self, policy: Dict[SolverState, int]):
        self.policy = policy

    def choose_move(
        self,
        board: Board,
        state: GameState,
        player_id: str,
        valid_moves: List[int],
    ) -> int:
        key = SolverState.from_game_state(state)
        move = self.policy.get(key)
        if move in valid_moves:
            return move
        # Fallback for unseen states
        return min(valid_moves)


class SerializedPolicyStrategy(Strategy):
    """Mr. X strategy backed by serialized keys dumped via --dump-policy.

    Expected key format:
        r=<round>|p=<player>|x=<mrx>|d=<d1,d2,...>
    """

    def __init__(self, serialized_policy: Dict[str, int]):
        self.serialized_policy = serialized_policy

    @staticmethod
    def _state_to_key(state: GameState) -> str:
        s = SolverState.from_game_state(state)
        return (
            f"r={s.round_number}|p={s.current_player}|"
            f"x={s.mrx_position}|d={','.join(map(str, s.detective_positions))}"
        )

    def choose_move(
        self,
        board: Board,
        state: GameState,
        player_id: str,
        valid_moves: List[int],
    ) -> int:
        key = self._state_to_key(state)
        move = self.serialized_policy.get(key)
        if move in valid_moves:
            return move
        return min(valid_moves)
