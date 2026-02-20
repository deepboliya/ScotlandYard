"""Board representation for Scotland Yard as a simple undirected graph."""

from typing import Dict, List, Set, Tuple


class Board:
    """Scotland Yard game board — a simple undirected graph.

    Attributes:
        nodes:     Sorted list of all node IDs.
        edges:     List of (u, v) edge tuples.
        positions: Dict mapping each node to (x, y) for visualization.
    """

    def __init__(
        self,
        edges: List[Tuple[int, int]],
        positions: Dict[int, Tuple[float, float]] | None = None,
    ):
        self.edges = list(edges)
        self._adjacency: Dict[int, Set[int]] = {}

        for u, v in self.edges:
            self._adjacency.setdefault(u, set()).add(v)
            self._adjacency.setdefault(v, set()).add(u)

        self.nodes: List[int] = sorted(self._adjacency.keys())
        self.positions = positions or {}

    # ---- queries --------------------------------------------------------

    def neighbors(self, node: int) -> Set[int]:
        """Return the set of neighbours for *node*."""
        return self._adjacency.get(node, set())

    def has_node(self, node: int) -> bool:
        return node in self._adjacency

    def has_edge(self, u: int, v: int) -> bool:
        return v in self._adjacency.get(u, set())

    def __contains__(self, node: int) -> bool:
        return self.has_node(node)

    def __repr__(self) -> str:
        return f"Board(nodes={len(self.nodes)}, edges={len(self.edges)})"


# ---- factory -----------------------------------------------------------


def create_top_right_board() -> Board:
    """Top-right portion of the real Scotland Yard board (nodes 1-20).

    Only edges whose *both* endpoints lie in {1, …, 20} are kept.
    Transport types are ignored — every edge is a simple connection.
    """
    edges = [
        (1, 8),   (1, 9),
        (2, 10),  (2, 20),
        (3, 4),   (3, 11),  (3, 12),
        (4, 13),
        (5, 15),  (5, 16),
        (6, 7),
        (7, 17),
        (8, 18),  (8, 19),
        (9, 19),  (9, 20),
        (10, 11),
        (13, 14),
        (14, 15),
        (15, 16),
    ]

    # Approximate (x, y) positions matching the real board layout.
    # x → right, y → up.
    positions: Dict[int, Tuple[float, float]] = {
        1:  (4.0, 10.0),
        8:  (3.0, 9.0),
        9:  (5.5, 9.0),
        20: (7.0, 9.0),
        2:  (8.5, 8.0),
        18: (1.5, 8.0),
        19: (4.5, 8.0),
        6:  (0.0, 7.0),
        7:  (1.0, 6.5),
        10: (8.0, 7.0),
        11: (7.0, 6.5),
        3:  (6.0, 5.5),
        12: (7.5, 5.0),
        17: (2.0, 5.5),
        4:  (5.5, 4.5),
        13: (4.5, 3.5),
        14: (3.5, 3.0),
        15: (2.5, 3.5),
        16: (1.5, 4.0),
        5:  (0.5, 4.5),
    }

    return Board(edges, positions)
