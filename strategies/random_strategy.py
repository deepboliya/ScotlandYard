"""Random strategy — picks a uniformly random valid move."""

import random
from typing import List

from game.board import Board
from game.state import GameState
from strategies.base import Strategy


class RandomStrategy(Strategy):
    """Baseline strategy that chooses a random valid move."""

    def __init__(self, seed: int | None = None):
        self.rng = random.Random(seed)

    def choose_move(
        self,
        board: Board,
        state: GameState,
        player_id: str,
        valid_moves: List[int],
    ) -> int:
        return self.rng.choice(valid_moves)
