"""Abstract base class for all player strategies."""

from abc import ABC, abstractmethod
from typing import List

from game.board import Board
from game.state import GameState


class Strategy(ABC):
    """Interface that every Mr. X / detective strategy must implement.

    Strategies are fully decoupled from both the game engine and the
    visualisation layer.  They receive the current game state and
    return a move choice.
    """

    @abstractmethod
    def choose_move(
        self,
        board: Board,
        state: GameState,
        player_id: str,
        valid_moves: List[int],
    ) -> int:
        """Pick a destination from *valid_moves*.

        Parameters
        ----------
        board : Board
            The game board (graph structure).
        state : GameState
            Current game state (positions, round, history, …).
        player_id : str
            ``"mrx"`` or ``"detective_0"``, ``"detective_1"``, …
        valid_moves : list[int]
            Legal destination nodes this turn.

        Returns
        -------
        int
            The chosen node — **must** be in *valid_moves*.
        """
        ...
