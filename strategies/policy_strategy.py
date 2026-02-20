"""Strategy that plays from a precomputed full-state policy map.

Important
---------
The **engine** increments ``round_number`` at the start of
``_step_mrx`` — *before* calling ``choose_move``.  The **solver**,
however, records Mr. X decisions at the *pre-increment* round number.
Both policy strategy classes therefore compensate by looking up Mr. X
states with ``round_number - 1``.
"""

from __future__ import annotations

from typing import Dict, List

from game.board import Board
from game.state import GameState
from solver.exhaustive_solver import SolverState
from strategies.base import Strategy


def _policy_lookup_state(state: GameState) -> SolverState:
    """Build the ``SolverState`` key that the solver would have used.

    For Mr. X turns the engine has already bumped ``round_number`` by 1,
    so we subtract it back to match the solver's indexing.  Detective
    turns are unaffected (the engine does not touch round_number there).
    """
    rn = state.round_number
    if state.current_player == "mrx":
        rn -= 1
    return SolverState(
        round_number=rn,
        current_player=state.current_player,
        mrx_position=state.mrx_position,
        detective_positions=tuple(state.detective_positions),
    )


class PolicyStrategy(Strategy):
    """Mr. X strategy backed by a state -> move mapping.

    Parameters
    ----------
    policy : dict[SolverState, int]
        Solver-produced state-to-move map.
    strict : bool
        If ``True``, raise ``KeyError`` when the policy has no entry
        instead of silently falling back to ``min(valid_moves)``.
    """

    def __init__(self, policy: Dict[SolverState, int], *, strict: bool = False):
        self.policy = policy
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


class SerializedPolicyStrategy(Strategy):
    """Mr. X strategy backed by serialized keys dumped via --dump-policy.

    Expected key format:
        r=<round>|p=<player>|x=<mrx>|d=<d1,d2,...>

    Parameters
    ----------
    serialized_policy : dict[str, int]
        Loaded JSON state-to-move map.
    strict : bool
        If ``True``, raise ``KeyError`` on lookup miss.
    """

    def __init__(self, serialized_policy: Dict[str, int], *, strict: bool = False):
        self.serialized_policy = serialized_policy
        self.strict = strict

    @staticmethod
    def _state_to_key(state: GameState) -> str:
        s = _policy_lookup_state(state)
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
        if self.strict:
            raise KeyError(
                f"SerializedPolicyStrategy: no policy entry for key "
                f"'{key}' (valid_moves={valid_moves})"
            )
        return min(valid_moves)
