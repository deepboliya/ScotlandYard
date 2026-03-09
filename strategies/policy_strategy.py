"""Strategy that plays from a precomputed full-state policy map.

Policy keys do not include the round number — they are purely
positional: (current_player, mrx_position, detective_positions).
"""

from __future__ import annotations

from typing import Dict, List, Tuple

from game.board import Board
from game.state import GameState
from old_solvers.exhaustive_solver import SolverState
from strategies.base import Strategy


def _policy_lookup_state(state: GameState) -> SolverState:
    """Build the ``SolverState`` key that the solver would have used."""
    return SolverState(
        round_number=0,  # round not used in policy keys
        current_player=state.current_player,
        mrx_position=state.mrx_position,
        detective_positions=tuple(state.detective_positions),
    )


class PolicyStrategy(Strategy):
    """Strategy backed by solver-produced state-to-move mappings.

    Parameters
    ----------
    policy : dict[SolverState, int]
        Mr. X state-to-move map.
    detective_policy : dict[SolverState, tuple[int, ...]], optional
        Detective state-to-joint-moves map.
    strict : bool
        If ``True``, raise ``KeyError`` when the policy has no entry
        instead of silently falling back to ``min(valid_moves)``.
    """

    def __init__(
        self,
        policy: Dict[SolverState, int],
        detective_policy: Dict[SolverState, Tuple[int, ...]] | None = None,
        *,
        strict: bool = False,
    ):
        self.policy = policy
        self.det_policy = detective_policy or {}
        self.strict = strict

    def choose_move(
        self,
        board: Board,
        state: GameState,
        player_id: str,
        valid_moves: List[int],
    ) -> int:
        key = _policy_lookup_state(state)
        move = self.policy.get(key)
        if move in valid_moves:
            return move
        if self.strict:
            raise KeyError(
                f"PolicyStrategy: no policy entry for {key!r} "
                f"(valid_moves={valid_moves})"
            )
        return min(valid_moves)

    def choose_detective_moves(
        self,
        board: Board,
        state: GameState,
        valid_moves_per_detective: List[List[int]],
    ) -> List[int]:
        key = _policy_lookup_state(state)
        combo = self.det_policy.get(key)
        if combo is not None and len(combo) == len(valid_moves_per_detective):
            if all(m in v for m, v in zip(combo, valid_moves_per_detective)):
                return list(combo)
        if self.strict:
            raise KeyError(
                f"PolicyStrategy: no detective policy entry for {key!r}"
            )
        # Fallback: each detective picks min valid move
        return [min(v) for v in valid_moves_per_detective]


class SerializedPolicyStrategy(Strategy):
    """Strategy backed by serialized keys dumped via --dump-policy.

    Expected key format:
        p=<player>|x=<mrx>|d=<d1,d2,...>

    Parameters
    ----------
    serialized_policy : dict[str, int]
        Loaded JSON Mr. X state-to-move map.
    serialized_det_policy : dict[str, list[int]], optional
        Loaded JSON detective state-to-joint-moves map.
    strict : bool
        If ``True``, raise ``KeyError`` on lookup miss.
    """

    def __init__(
        self,
        serialized_policy: Dict[str, int],
        serialized_det_policy: Dict[str, list] | None = None,
        *,
        strict: bool = False,
    ):
        self.serialized_policy = serialized_policy
        self.serialized_det_policy = serialized_det_policy or {}
        self.strict = strict

    @staticmethod
    def _state_to_key(state: GameState) -> str:
        s = _policy_lookup_state(state)
        return (
            f"p={s.current_player}|"
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
        if self.strict:
            raise KeyError(
                f"SerializedPolicyStrategy: no policy entry for key "
                f"'{key}' (valid_moves={valid_moves})"
            )
        return min(valid_moves)

    def choose_detective_moves(
        self,
        board: Board,
        state: GameState,
        valid_moves_per_detective: List[List[int]],
    ) -> List[int]:
        key = self._state_to_key(state)
        combo = self.serialized_det_policy.get(key)
        if combo is not None and len(combo) == len(valid_moves_per_detective):
            if all(m in v for m, v in zip(combo, valid_moves_per_detective)):
                return list(combo)
        if self.strict:
            raise KeyError(
                f"SerializedPolicyStrategy: no detective policy entry for key "
                f"'{key}'"
            )
        return [min(v) for v in valid_moves_per_detective]
