"""Board representation for Scotland Yard as a simple undirected graph."""

from pathlib import Path
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
        map_path: str | None = None,
    ):
        self.edges = list(edges)
        self.map_path = map_path
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
        return f"Board(map_path={self.map_path}, nodes={len(self.nodes)}, edges={len(self.edges)})"


# ---- factory -----------------------------------------------------------


def _load_edges_from_map_file(file_path: Path) -> List[Tuple[int, int]]:
    """Load undirected edges from a map file.

    Input format is one edge per line:
    ``u v transport_type`` (transport type ignored for now).
    """
    edge_set: Set[Tuple[int, int]] = set()

    with file_path.open("r", encoding="utf-8") as f:
        for raw in f:
            line = raw.strip()
            if not line:
                continue

            parts = line.split()
            if len(parts) < 2:
                continue

            u, v = int(parts[0]), int(parts[1])
            if u == v:
                continue

            a, b = (u, v) if u < v else (v, u)
            edge_set.add((a, b))

    return sorted(edge_set)


def _load_positions_from_csv(file_path: Path) -> Dict[int, Tuple[float, float]]:
    """Load node (x, y) positions from a CSV file.

    Expected format (header row then ``node_id, x, y`` per line).
    The y-axis is flipped so that the board renders correctly in
    matplotlib (image y grows down, matplotlib y grows up).
    """
    positions: Dict[int, Tuple[float, float]] = {}
    with file_path.open("r", encoding="utf-8") as f:
        header = True
        for raw in f:
            if header:
                header = False
                continue
            line = raw.strip()
            if not line:
                continue
            parts = [p.strip() for p in line.split(",")]
            if len(parts) < 3:
                continue
            node_id = int(parts[0])
            x = float(parts[1])
            y = -float(parts[2])  # flip y for matplotlib
            positions[node_id] = (x, y)
    return positions


def create_board_from_map(map_path: str) -> Board:
    """Create a board from a map file path.

    Parameters
    ----------
    map_path:
        Path to a map file where each line has at least two columns:
        ``u v [ticket_type]``. Ticket type is ignored.
    """
    map_file = Path(map_path)
    if not map_file.is_absolute():
        map_file = Path(__file__).resolve().parent.parent / map_file

    edges = _load_edges_from_map_file(map_file)

    # Try to load node positions from node_locations.csv next to the map file
    positions_file = map_file.parent / "node_locations.csv"
    positions = None
    if positions_file.exists():
        all_positions = _load_positions_from_csv(positions_file)
        # Only keep positions for nodes that actually appear in the edges
        edge_nodes = set()
        for u, v in edges:
            edge_nodes.add(u)
            edge_nodes.add(v)
        positions = {n: xy for n, xy in all_positions.items() if n in edge_nodes}

    return Board(edges=edges, positions=positions, map_path=str(map_file))
