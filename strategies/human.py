"""Human strategy — delegates move selection to a human player.

The strategy accepts a *move_selector* callback so it remains decoupled
from any particular UI (CLI, matplotlib clicks, web, etc.).
"""

from __future__ import annotations

from typing import Callable, List, Optional

from game.board import Board
from game.state import GameState
from strategies.base import Strategy


class HumanStrategy(Strategy):
    """Strategy driven by a human player.

    Parameters
    ----------
    move_selector : callable, optional
        ``move_selector(player_id, valid_moves) -> int``.
        If ``None``, falls back to a simple CLI prompt.
    """

    def __init__(
        self,
        move_selector: Optional[Callable[[str, List[int]], int]] = None,
    ):
        self.move_selector = move_selector or self._cli_selector

    # ---- fallback CLI ---------------------------------------------------

    @staticmethod
    def _cli_selector(player_id: str, valid_moves: List[int]) -> int:
        label = player_id.replace("_", " ").title()
        print(f"\n--- {label}'s turn ---")
        print(f"  Valid moves: {valid_moves}")
        while True:
            try:
                move = int(input("  Enter node number: "))
                if move in valid_moves:
                    return move
                print(f"  Invalid! Choose from {valid_moves}")
            except (ValueError, EOFError):
                print("  Please enter a valid number.")

    # ---- strategy interface ---------------------------------------------

    def choose_move(
        self,
        board: Board,
        state: GameState,
        player_id: str,
        valid_moves: List[int],
    ) -> int:
        return self.move_selector(player_id, valid_moves)
