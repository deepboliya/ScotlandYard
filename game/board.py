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
    """Extended top-right style board with local + long-range links.

    This starts from the original 1-20 subgraph and adds more nodes
    with a few far-reaching "underground-like" edges. Transport types
    are still ignored — every edge is a simple connection.
    """
    edges = [
        # base local graph (original 1-20)
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

        # extended local region (21-35)
        (20, 21), (21, 22), (22, 23), (23, 24),
        (24, 25), (25, 26), (26, 27), (27, 28),
        (19, 29), (29, 30), (30, 31), (31, 32),
        (30, 33), (31, 34), (32, 35),
        (33, 34), (34, 35),
        (11, 23), (12, 25), (2, 24), (21, 29),
        (25, 30), (26, 31), (28, 32),

        # far-reaching ("underground-like") connections
        (1, 24), (2, 29), (3, 28), (5, 30),
        (6, 22), (9, 27), (12, 33), (14, 34),
        (17, 31), (18, 35),
    ]

    # Approximate (x, y) positions.
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

        # extended area to the right
        21: (9.0, 9.0),
        22: (10.2, 8.1),
        23: (11.4, 7.2),
        24: (12.6, 8.3),
        25: (13.0, 6.4),
        26: (14.0, 5.6),
        27: (14.8, 7.0),
        28: (15.7, 6.0),
        29: (9.6, 7.3),
        30: (10.8, 6.0),
        31: (12.0, 5.0),
        32: (13.6, 4.4),
        33: (10.7, 4.7),
        34: (12.0, 3.6),
        35: (13.5, 3.2),
    }

    return Board(edges, positions)
