"""Game engine — manages game flow, validates moves, checks win conditions.

The engine is the central coordinator.  It does **not** contain any
strategy logic; strategies are injected as constructor dependencies.

Detectives move as a group: after Mr. X moves, all detectives choose
their destinations simultaneously in a single step.
"""

from __future__ import annotations

from typing import Callable, List, Optional

from game.board import Board
from game.state import GameState
from strategies.base import Strategy


class GameEngine:
    """Turn-by-turn game engine for Scotland Yard.

    Parameters
    ----------
    board : Board
        The game board graph.
    state : GameState
        Initial (mutable) game state.
    mrx_strategy : Strategy
        Strategy that drives Mr. X's decisions.
    detective_strategy : Strategy
        A single strategy that controls **all** detectives jointly.
    on_move : callable, optional
        ``on_move(player_id, from_node, to_node)`` called after every move.
    """

    def __init__(
        self,
        board: Board,
        state: GameState,
        mrx_strategy: Strategy,
        detective_strategy: Strategy,
        on_move: Optional[Callable] = None,
    ):
        self.board = board
        self.state = state
        self.mrx_strategy = mrx_strategy
        self.detective_strategy = detective_strategy
        self.on_move = on_move

    # ---- move helpers ---------------------------------------------------

    def get_valid_moves(
        self, node: int, excluded_nodes: List[int] | None = None
    ) -> List[int]:
        """Valid destinations from *node*, excluding *excluded_nodes*."""
        neighbors = self.board.neighbors(node)
        if excluded_nodes:
            excluded = set(excluded_nodes)
            return sorted(n for n in neighbors if n not in excluded)
        return sorted(neighbors)

    def get_mrx_valid_moves(self) -> List[int]:
        """Valid moves for Mr. X (cannot move onto a detective's node)."""
        s = self.state
        return self.get_valid_moves(s.mrx_position, s.detective_positions)

    def get_detective_valid_moves(self) -> List[List[int]]:
        """Valid moves for each detective.

        Each detective can move to any neighbour not occupied by another
        detective (Mr. X's node *is* a valid destination — that's a
        capture).
        """
        s = self.state
        result: List[List[int]] = []
        for idx in range(s.num_detectives):
            moves = self.get_valid_moves(s.detective_positions[idx])
            if not moves:
                moves = [s.detective_positions[idx]]  # stuck — stay put
            result.append(moves)
        return result

    def get_current_valid_moves(self) -> List[int]:
        """Valid moves for whoever's turn it currently is.

        For Mr. X returns a flat list.  For detectives this returns
        the valid moves for detective_0 only (used by some UI code
        that needs a single list — prefer ``get_detective_valid_moves``
        for full info).
        """
        s = self.state
        if s.is_mrx_turn:
            return self.get_mrx_valid_moves()
        # Legacy: return first detective's valid moves
        per_det = self.get_detective_valid_moves()
        return per_det[0] if per_det else []

    # ---- win / loss -----------------------------------------------------

    def _check_game_over(self) -> bool:
        """Update ``state.game_over`` and return whether the game ended."""
        s = self.state

        # Caught — a detective occupies Mr. X's node.
        if s.mrx_position in s.detective_positions:
            s.game_over = True
            s.mrx_caught = True
            return True

        # Survived — all rounds done and it's Mr. X's turn again.
        if s.round_number >= s.max_rounds and s.is_mrx_turn:
            s.game_over = True
            s.mrx_caught = False
            return True

        # Trapped — Mr. X has no valid move on his turn.
        if s.is_mrx_turn:
            if not self.get_valid_moves(s.mrx_position, s.detective_positions):
                s.game_over = True
                s.mrx_caught = True
                return True

        return False

    # ---- stepping -------------------------------------------------------

    def step(self) -> Optional[int | List[int]]:
        """Execute **one side's** move and advance to the next side.

        For Mr. X returns the destination node.
        For detectives returns a list of destination nodes.
        Returns ``None`` if the game is already over.
        """
        s = self.state
        if s.game_over or self._check_game_over():
            return None

        if s.is_mrx_turn:
            move = self._step_mrx()
        else:
            move = self._step_detectives()

        self._check_game_over()
        return move

    def _step_mrx(self) -> int:
        s = self.state
        s.round_number += 1

        valid = self.get_valid_moves(s.mrx_position, s.detective_positions)
        if not valid:
            s.game_over = True
            s.mrx_caught = True
            return s.mrx_position

        from_node = s.mrx_position
        move = self.mrx_strategy.choose_move(self.board, s, "mrx", valid)
        assert move in valid, f"Invalid Mr. X move {move}; valid = {valid}"

        s.mrx_position = move
        s.mrx_history.append(move)

        # Advance turn to detectives (or back to mrx if none).
        s.current_player = "detectives" if s.num_detectives else "mrx"

        if self.on_move:
            self.on_move("mrx", from_node, move)
        return move

    def _step_detectives(self) -> List[int]:
        """Move all detectives at once."""
        s = self.state

        valid_per_det = self.get_detective_valid_moves()
        from_nodes = list(s.detective_positions)

        moves = self.detective_strategy.choose_detective_moves(
            self.board, s, valid_per_det
        )

        assert len(moves) == s.num_detectives, (
            f"Expected {s.num_detectives} moves, got {len(moves)}"
        )
        for i, (move, valid) in enumerate(zip(moves, valid_per_det)):
            assert move in valid, (
                f"Invalid detective_{i} move {move}; valid = {valid}"
            )

        for i, move in enumerate(moves):
            s.detective_positions[i] = move

        # Keep positions sorted (detectives are interchangeable).
        s.detective_positions.sort()

        # Advance to Mr. X's turn.
        s.current_player = "mrx"

        if self.on_move:
            for i, (frm, to) in enumerate(zip(from_nodes, moves)):
                self.on_move(f"detective_{i}", frm, to)
        return moves

    # ---- convenience ----------------------------------------------------

    def play_round(self) -> None:
        """Play one complete round (Mr. X + all detectives)."""
        if self.state.game_over:
            return
        self.step()  # Mr. X
        if not self.state.game_over:
            self.step()  # all detectives at once

    def play_game(self) -> GameState:
        """Play until the game is over and return the final state."""
        while not self.state.game_over:
            self.play_round()
        return self.state
