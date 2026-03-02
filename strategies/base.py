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

    For Mr. X, ``choose_move`` returns a single destination.
    For detectives, ``choose_detective_moves`` returns one destination
    per detective simultaneously.
    """

    @abstractmethod
    def choose_move(
        self,
        board: Board,
        state: GameState,
        player_id: str,
        valid_moves: List[int],
    ) -> int:
        """Pick a destination from *valid_moves* (used for Mr. X).

        Parameters
        ----------
        board : Board
            The game board (graph structure).
        state : GameState
            Current game state (positions, round, history, …).
        player_id : str
            ``"mrx"`` or ``"detectives"``.
        valid_moves : list[int]
            Legal destination nodes this turn.

        Returns
        -------
        int
            The chosen node — **must** be in *valid_moves*.
        """
        ...

    def choose_detective_moves(
        self,
        board: Board,
        state: GameState,
        valid_moves_per_detective: List[List[int]],
    ) -> List[int]:
        """Pick a destination for every detective simultaneously.

        Parameters
        ----------
        board : Board
            The game board (graph structure).
        state : GameState
            Current game state (positions, round, history, …).
        valid_moves_per_detective : list[list[int]]
            ``valid_moves_per_detective[i]`` is the list of legal
            destinations for detective *i*.

        Returns
        -------
        list[int]
            One destination per detective.  ``result[i]`` **must** be
            in ``valid_moves_per_detective[i]``.
        """
        # Default: choose each detective's move independently via
        # choose_move (backwards compatible with simple strategies).
        return [
            self.choose_move(board, state, f"detective_{i}", moves)
            for i, moves in enumerate(valid_moves_per_detective)
        ]
