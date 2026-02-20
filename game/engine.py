"""Game engine — manages game flow, validates moves, checks win conditions.

The engine is the central coordinator.  It does **not** contain any
strategy logic; strategies are injected as constructor dependencies.
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
    detective_strategies : list[Strategy]
        One strategy per detective (same order as ``state.detective_positions``).
    on_move : callable, optional
        ``on_move(player_id, from_node, to_node)`` called after every move.
    """

    def __init__(
        self,
        board: Board,
        state: GameState,
        mrx_strategy: Strategy,
        detective_strategies: List[Strategy],
        on_move: Optional[Callable] = None,
    ):
        self.board = board
        self.state = state
        self.mrx_strategy = mrx_strategy
        self.detective_strategies = detective_strategies
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

    def get_current_valid_moves(self) -> List[int]:
        """Valid moves for whoever's turn it currently is."""
        s = self.state
        if s.is_mrx_turn:
            return self.get_valid_moves(s.mrx_position, s.detective_positions)
        idx = int(s.current_player.split("_")[1])
        occupied = [
            s.detective_positions[i]
            for i in range(s.num_detectives)
            if i != idx
        ]
        return self.get_valid_moves(s.detective_positions[idx], occupied)

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

    def step(self) -> Optional[int]:
        """Execute **one player's** move and advance to the next player.

        Returns the destination node, or ``None`` if the game is already over.
        """
        s = self.state
        if s.game_over or self._check_game_over():
            return None

        if s.is_mrx_turn:
            move = self._step_mrx()
        else:
            move = self._step_detective()

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

        # Advance turn to first detective (or back to mrx if none).
        s.current_player = "detective_0" if s.num_detectives else "mrx"

        if self.on_move:
            self.on_move("mrx", from_node, move)
        return move

    def _step_detective(self) -> int:
        s = self.state
        idx = int(s.current_player.split("_")[1])

        occupied = [
            s.detective_positions[i]
            for i in range(s.num_detectives)
            if i != idx
        ]
        valid = self.get_valid_moves(s.detective_positions[idx], occupied)

        from_node = s.detective_positions[idx]
        if valid:
            strategy = self.detective_strategies[idx]
            move = strategy.choose_move(
                self.board, s, f"detective_{idx}", valid
            )
            assert move in valid, (
                f"Invalid detective_{idx} move {move}; valid = {valid}"
            )
            s.detective_positions[idx] = move
        else:
            move = from_node  # stuck — stay put

        # Advance to next detective or back to Mr. X.
        if idx + 1 < s.num_detectives:
            s.current_player = f"detective_{idx + 1}"
        else:
            s.current_player = "mrx"

        if self.on_move:
            self.on_move(f"detective_{idx}", from_node, move)
        return move

    # ---- convenience ----------------------------------------------------

    def play_round(self) -> None:
        """Play one complete round (Mr. X + every detective)."""
        if self.state.game_over:
            return
        self.step()  # Mr. X
        for _ in range(self.state.num_detectives):
            if self.state.game_over:
                return
            self.step()

    def play_game(self) -> GameState:
        """Play until the game is over and return the final state."""
        while not self.state.game_over:
            self.play_round()
        return self.state
