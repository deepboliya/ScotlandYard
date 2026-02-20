"""Interactive Scotland Yard visualiser built on matplotlib + networkx.

Two usage modes
---------------
* **Observer** (``run``): watch AI strategies play.
  Press **N** step · **R** round · **A** auto-play · **Q** quit.
* **Interactive** (``run_interactive``): play as Mr. X by clicking on
  highlighted nodes; detectives are driven by their strategy.
"""

from __future__ import annotations

import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
import networkx as nx
from typing import List, Optional

from game.engine import GameEngine


class GameVisualizer:
    """Draws the board, player tokens, and handles user interaction.

    Parameters
    ----------
    engine : GameEngine
        A fully-configured game engine (board + state + strategies).
    figsize : tuple[int, int]
        Figure size in inches.
    """

    # ── colour palette ──────────────────────────────────────────────────
    CLR_MRX        = "#e74c3c"
    CLR_MRX_EDGE   = "#c0392b"
    CLR_DET        = "#3498db"
    CLR_DET_EDGE   = "#2471a3"
    CLR_VALID      = "#2ecc71"
    CLR_VALID_EDGE = "#27ae60"
    CLR_NODE       = "#ecf0f1"
    CLR_NODE_EDGE  = "#95a5a6"
    CLR_EDGE       = "#bdc3c7"
    CLR_BG         = "#fafafa"

    def __init__(
        self,
        engine: GameEngine,
        figsize: tuple = (12, 10),
        mode_label: str = "Unknown",
        mrx_policy_label: str = "Unknown",
        detective_policy_label: str = "Unknown",
    ):
        self.engine = engine
        self.mode_label = mode_label
        self.mrx_policy_label = mrx_policy_label
        self.detective_policy_label = detective_policy_label

        # networkx graph (purely for drawing)
        self.G = nx.Graph()
        self.G.add_nodes_from(engine.board.nodes)
        self.G.add_edges_from(engine.board.edges)

        # interaction state
        self._valid_moves: List[int] = []
        self._selected_node: Optional[int] = None
        self._auto_delay = 0.5

        # matplotlib figure
        self.fig, self.ax = plt.subplots(1, 1, figsize=figsize)
        self.fig.patch.set_facecolor(self.CLR_BG)
        try:
            self.fig.canvas.manager.set_window_title("Scotland Yard")
        except AttributeError:
            pass  # some backends lack set_window_title

        # event wiring
        self.fig.canvas.mpl_connect("key_press_event", self._on_key)
        self.fig.canvas.mpl_connect("button_press_event", self._on_click)

    # ── drawing ─────────────────────────────────────────────────────────

    def draw(self) -> None:
        """Render the current game state onto the axes."""
        self.ax.clear()
        s = self.engine.state
        pos = self.engine.board.positions

        # edges
        nx.draw_networkx_edges(
            self.G, pos, ax=self.ax,
            edge_color=self.CLR_EDGE, width=2.5, alpha=0.7,
        )

        # categorise nodes
        mrx = s.mrx_position
        det_set = set(s.detective_positions)
        valid_set = set(self._valid_moves)
        regular = [
            n for n in self.engine.board.nodes
            if n != mrx and n not in det_set
        ]

        # regular / valid-move nodes
        nx.draw_networkx_nodes(
            self.G, pos, nodelist=regular, ax=self.ax,
            node_color=[
                self.CLR_VALID if n in valid_set else self.CLR_NODE
                for n in regular
            ],
            node_size=550,
            edgecolors=[
                self.CLR_VALID_EDGE if n in valid_set else self.CLR_NODE_EDGE
                for n in regular
            ],
            linewidths=[3 if n in valid_set else 1.5 for n in regular],
        )

        # Mr. X
        nx.draw_networkx_nodes(
            self.G, pos, nodelist=[mrx], ax=self.ax,
            node_color=self.CLR_MRX, node_size=750,
            edgecolors=self.CLR_MRX_EDGE, linewidths=2.5,
        )

        # detectives
        det_list = sorted(det_set)
        if det_list:
            nx.draw_networkx_nodes(
                self.G, pos, nodelist=det_list, ax=self.ax,
                node_color=self.CLR_DET, node_size=750,
                edgecolors=self.CLR_DET_EDGE, linewidths=2.5,
            )

        # labels (white on coloured nodes, black on grey)
        for node, (x, y) in pos.items():
            colour = (
                "white"
                if node == mrx or node in det_set
                else "black"
            )
            self.ax.text(
                x, y, str(node),
                ha="center", va="center",
                fontsize=11, fontweight="bold", color=colour,
                zorder=5,
            )

        # title
        if s.game_over:
            title = f"GAME OVER — {s.result_str}"
            title_clr = self.CLR_MRX if not s.mrx_caught else self.CLR_DET
        else:
            player = s.current_player.replace("_", " ").title()
            title = f"Round {s.round_number} │ {player}'s Turn"
            title_clr = "black"

        self.ax.set_title(
            title, fontsize=15, fontweight="bold",
            color=title_clr, pad=15,
        )

        # info bar
        info = (
            f"Mr. X: node {s.mrx_position}  │  "
            f"Detectives: {s.detective_positions}  │  "
            f"Round {s.round_number}/{s.max_rounds}"
        )
        self.ax.text(
            0.02, -0.02, info,
            transform=self.ax.transAxes, ha="left",
            fontsize=9, color="gray",
        )

        # policy/mode panel
        policy_text = (
            f"Mode: {self.mode_label}\n"
            f"Mr. X policy: {self.mrx_policy_label}\n"
            f"Detective policy: {self.detective_policy_label}"
        )
        self.ax.text(
            0.02,
            0.98,
            policy_text,
            transform=self.ax.transAxes,
            ha="left",
            va="top",
            fontsize=9,
            color="#2c3e50",
            bbox={"boxstyle": "round,pad=0.35", "facecolor": "white", "alpha": 0.88, "edgecolor": "#d0d7de"},
        )

        # help bar
        if self._valid_moves:
            help_txt = "Click a green node to move  │  [Q] Quit"
        else:
            help_txt = "[N] Step  [R] Round  [A] Auto  [Q] Quit"
        self.ax.text(
            0.98, -0.02, help_txt,
            transform=self.ax.transAxes, ha="right",
            fontsize=9, color="gray",
        )

        # legend
        legend = [
            mpatches.Patch(
                facecolor=self.CLR_MRX, edgecolor=self.CLR_MRX_EDGE,
                label="Mr. X", linewidth=1.5,
            ),
            mpatches.Patch(
                facecolor=self.CLR_DET, edgecolor=self.CLR_DET_EDGE,
                label="Detective", linewidth=1.5,
            ),
            mpatches.Patch(
                facecolor=self.CLR_VALID, edgecolor=self.CLR_VALID_EDGE,
                label="Valid Move", linewidth=1.5,
            ),
        ]
        self.ax.legend(handles=legend, loc="lower right", fontsize=10,
                       framealpha=0.9)

        self.ax.set_aspect("equal")
        self.ax.axis("off")
        self.fig.tight_layout()
        self.fig.canvas.draw_idle()

    # ── node picking ────────────────────────────────────────────────────

    def _closest_node(self, x: float, y: float, threshold: float = 0.6):
        """Return the node closest to *(x, y)*, or ``None``."""
        best, best_d = None, float("inf")
        for node, (nx_, ny) in self.engine.board.positions.items():
            d = ((x - nx_) ** 2 + (y - ny) ** 2) ** 0.5
            if d < best_d:
                best, best_d = node, d
        return best if best_d <= threshold else None

    # ── event handlers ──────────────────────────────────────────────────

    def _on_click(self, event) -> None:
        if event.inaxes != self.ax:
            return
        node = self._closest_node(event.xdata, event.ydata)
        if node is not None and node in self._valid_moves:
            self._selected_node = node

    def _on_key(self, event) -> None:
        if event.key == "n":
            if not self.engine.state.game_over:
                self.engine.step()
                self.draw()
        elif event.key == "r":
            if not self.engine.state.game_over:
                self.engine.play_round()
                self.draw()
        elif event.key == "a":
            self._auto_play()
        elif event.key == "q":
            plt.close(self.fig)

    # ── run modes ───────────────────────────────────────────────────────

    def run(self) -> None:
        """Observer mode — advance the game with keyboard shortcuts."""
        self.draw()
        plt.show()

    def run_interactive(self) -> None:
        """Interactive mode — play as Mr. X via mouse clicks.

        Detectives are driven by their assigned strategies automatically.
        """
        plt.ion()
        self.fig.show()
        self.draw()
        plt.pause(0.1)

        try:
            while not self.engine.state.game_over:
                # Mr. X's turn → wait for human click inside engine.step()
                # via the HumanStrategy → wait_for_click callback.
                self.engine.step()
                self.draw()

                # brief pause so the user can see each intermediate state
                if not self.engine.state.is_mrx_turn:
                    plt.pause(0.4)
                else:
                    plt.pause(0.2)

        except SystemExit:
            return

        # show final state
        self.draw()
        print(f"\n{'=' * 44}")
        print(f"  {self.engine.state.result_str}")
        print(f"  Final round: {self.engine.state.round_number}")
        print(f"{'=' * 44}")
        plt.ioff()
        plt.show()

    # ── helpers used by HumanStrategy callback ──────────────────────────

    def wait_for_click(
        self, player_id: str, valid_moves: List[int]
    ) -> int:
        """Highlight *valid_moves* and block until the user clicks one.

        Intended to be passed as ``move_selector`` to
        :class:`strategies.human.HumanStrategy`.
        """
        self._valid_moves = list(valid_moves)
        self._selected_node = None
        self.draw()

        while self._selected_node is None:
            if not plt.fignum_exists(self.fig.number):
                raise SystemExit("Window closed")
            plt.pause(0.05)

        move = self._selected_node
        self._valid_moves = []
        self._selected_node = None
        return move

    # ── auto-play helper ────────────────────────────────────────────────

    def _auto_play(self) -> None:
        while not self.engine.state.game_over:
            self.engine.step()
            self.draw()
            plt.pause(self._auto_delay)
