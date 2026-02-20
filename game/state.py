"""Game state for Scotland Yard."""

from __future__ import annotations

from copy import deepcopy
from dataclasses import dataclass, field
from typing import List, Optional, Tuple


@dataclass
class GameState:
    """Full state of a Scotland Yard game.

    Attributes:
        mrx_position:       Current node of Mr. X.
        detective_positions: Current nodes of each detective.
        round_number:       Current round (0 = game hasn't started).
        current_player:     ``"mrx"`` or ``"detective_<i>"``.
        mrx_history:        Mr. X's position after each round.
        reveal_rounds:      Rounds on which Mr. X must reveal his position.
        game_over:          Whether the game has ended.
        mrx_caught:         Whether Mr. X was caught (detectives win).
        max_rounds:         Maximum rounds before Mr. X wins by survival.
    """

    mrx_position: int
    detective_positions: List[int]
    round_number: int = 0
    current_player: str = "mrx"
    mrx_history: List[int] = field(default_factory=list)
    reveal_rounds: Tuple[int, ...] = (3, 8, 13)
    game_over: bool = False
    mrx_caught: bool = False
    max_rounds: int = 15

    # ---- derived properties ---------------------------------------------

    @property
    def num_detectives(self) -> int:
        return len(self.detective_positions)

    @property
    def is_mrx_turn(self) -> bool:
        return self.current_player == "mrx"

    @property
    def is_mrx_revealed(self) -> bool:
        """True if Mr. X's position is publicly known this round."""
        return self.round_number in self.reveal_rounds

    @property
    def mrx_last_known_position(self) -> Optional[int]:
        """Last position where Mr. X was revealed, or ``None``."""
        for r in sorted(self.reveal_rounds, reverse=True):
            idx = r - 1  # mrx_history is 0-indexed; round 1 → index 0
            if r <= self.round_number and 0 <= idx < len(self.mrx_history):
                return self.mrx_history[idx]
        return None

    @property
    def result_str(self) -> str:
        if not self.game_over:
            return "In progress"
        return "Detectives win!" if self.mrx_caught else "Mr. X escapes!"

    # ---- helpers --------------------------------------------------------

    def copy(self) -> GameState:
        """Deep-copy the entire state."""
        return deepcopy(self)

    def __repr__(self) -> str:
        return (
            f"GameState(round={self.round_number}, "
            f"player={self.current_player}, "
            f"mrx={self.mrx_position}, "
            f"detectives={self.detective_positions})"
        )
