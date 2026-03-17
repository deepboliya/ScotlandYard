"""Interactive Scotland Yard visualiser built on matplotlib + networkx.

Interactive mode (``run_interactive``): play as either Mr. X or detectives
by clicking on highlighted nodes.
Press **N** to play the best policy move · **B** undo · **Q** quit.
"""

from __future__ import annotations

import math
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
import networkx as nx
from typing import List, Optional

from game.engine import GameEngine


class _UndoRequested(Exception):
    """Raised inside wait_for_click when the user presses undo."""


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
        guaranteed_survival: int | None = None,
        survival_fn=None,
        mrx_policy: dict | None = None,
        det_policy: dict | None = None,
    ):
        self.engine = engine
        self.mode_label = mode_label
        self.mrx_policy_label = mrx_policy_label
        self.detective_policy_label = detective_policy_label
        self.guaranteed_survival = guaranteed_survival
        self.survival_fn = survival_fn
        self.mrx_policy = mrx_policy
        self.det_policy = det_policy

        # networkx graph (purely for drawing)
        self.G = nx.Graph()
        self.G.add_nodes_from(engine.board.nodes)
        self.G.add_edges_from(engine.board.edges)
        self._draw_pos = self._build_draw_positions()
        self._pick_threshold = self._estimate_pick_threshold(self._draw_pos)

        # interaction state
        self._valid_moves: List[int] = []
        self._selected_node: Optional[int] = None
        self._undo_requested = False
        self._auto_delay = 0.5
        self._current_player_id: Optional[str] = None
        self._detective_manual_move_started = False  # True if human clicked for any detective this turn
        self._detective_auto_moves: Optional[List[int]] = None  # Stores all detective moves when N is pressed
        self._auto_move_used = False  # True when N was pressed to auto-play

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

    def _build_draw_positions(self):
        """Use board positions when every node has one, else spring layout."""
        board_pos = self.engine.board.positions
        if board_pos and all(n in board_pos for n in self.engine.board.nodes):
            return board_pos

        # Fallback: spring layout
        n_nodes = len(self.engine.board.nodes)
        init = board_pos if board_pos else None
        k = 1.2 / max(1.0, math.sqrt(n_nodes))
        return nx.spring_layout(
            self.G,
            pos=init,
            seed=11,
            iterations=400,
            k=k,
        )

    @staticmethod
    def _estimate_pick_threshold(pos) -> float:
        """Adaptive click radius from nearest-neighbour spacing."""
        pts = list(pos.values())
        if len(pts) < 2:
            return 0.6

        nearest_distances: List[float] = []
        for i, (x1, y1) in enumerate(pts):
            best = float("inf")
            for j, (x2, y2) in enumerate(pts):
                if i == j:
                    continue
                d = ((x1 - x2) ** 2 + (y1 - y2) ** 2) ** 0.5
                if d < best:
                    best = d
            nearest_distances.append(best)

        nearest_distances.sort()
        median = nearest_distances[len(nearest_distances) // 2]
        return median * 0.45

    def draw(self) -> None:
        """Render the current game state onto the axes."""
        self.ax.clear()

        # Remove any previous figure-level texts (info bar, mode panel, help)
        for txt in getattr(self, '_fig_texts', []):
            txt.remove()
        self._fig_texts = []

        s = self.engine.state
        pos = self._draw_pos

        # edges
        nx.draw_networkx_edges(
            self.G, pos, ax=self.ax,
            edge_color=self.CLR_EDGE, width=1.8, alpha=0.55,
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
            player = "Mr. X" if s.current_player == "mrx" else "Detectives"
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
        self._fig_texts.append(self.fig.text(
            0.5, 0.01, info,
            ha="center", va="bottom",
            fontsize=9, color="gray",
        ))

        # policy/mode panel (top-right, outside the graph area)
        policy_text = (
            f"Mode: {self.mode_label}\n"
            f"Mr. X policy: {self.mrx_policy_label}\n"
            f"Detective policy: {self.detective_policy_label}"
        )
        if self.survival_fn is not None and not s.game_over:
            surv_now = self.survival_fn(s)
            if surv_now is not None:
                max_r = s.max_rounds
                whose = "Mr. X" if s.current_player == "mrx" else "Detectives"
                # Convert relative survival to absolute total rounds
                if s.current_player == "mrx":
                    absolute = (s.round_number - 1) + surv_now
                else:
                    absolute = s.round_number + surv_now
                if absolute >= max_r:
                    surv_line = f"Mr. X guaranteed escape ({absolute}/{max_r} rounds)"
                else:
                    surv_line = f"Mr. X survives {absolute}/{max_r} rounds"
                policy_text += f"\n{surv_line}"
                policy_text += f"\nFrom here: X can survive {surv_now} more round{'s' if surv_now != 1 else ''} ({whose}'s turn)"
            elif self.guaranteed_survival is not None:
                max_r = s.max_rounds
                gs = self.guaranteed_survival
                if gs >= max_r:
                    surv_line = f"Mr. X guaranteed escape ({gs}/{max_r} rounds)"
                else:
                    surv_line = f"Mr. X survives {gs}/{max_r} rounds (detectives win)"
                policy_text += f"\n{surv_line}"
        elif self.guaranteed_survival is not None:
            max_r = s.max_rounds
            gs = self.guaranteed_survival
            if gs >= max_r:
                surv_line = f"Mr. X guaranteed escape ({gs}/{max_r} rounds)"
            else:
                surv_line = f"Mr. X survives {gs}/{max_r} rounds (detectives win)"
            policy_text += f"\n{surv_line}"
        self._fig_texts.append(self.fig.text(
            0.99,
            0.99,
            policy_text,
            ha="right",
            va="top",
            fontsize=8,
            color="#2c3e50",
            bbox={"boxstyle": "round,pad=0.35", "facecolor": "white", "alpha": 0.88, "edgecolor": "#d0d7de"},
        ))

        # help bar
        if self._valid_moves:
            help_txt = "Click a green node to move  │  [B] Undo  │  [Q] Quit"
        else:
            help_txt = "[N] Step  [R] Round  [B] Undo  [A] Auto  [Q] Quit"
        self._fig_texts.append(self.fig.text(
            0.99, 0.01, help_txt,
            ha="right", va="bottom",
            fontsize=9, color="gray",
        ))

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

    def _closest_node(self, x: float, y: float, threshold: float | None = None):
        """Return the node closest to *(x, y)*, or ``None``."""
        if threshold is None:
            threshold = self._pick_threshold
        best, best_d = None, float("inf")
        for node, (nx_, ny) in self._draw_pos.items():
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
            # Mark that human made a manual move for a detective
            if self._current_player_id and self._current_player_id.startswith("detective_"):
                self._detective_manual_move_started = True

    @property
    def _waiting_for_click(self) -> bool:
        """True when wait_for_click is blocking for user input."""
        return bool(self._valid_moves)

    def _on_key(self, event) -> None:
        # When waiting for a human click, allow N (play best move), undo, and quit.
        if self._waiting_for_click:
            if event.key == "n":
                # Block N if human already started making detective moves manually
                if self._detective_manual_move_started:
                    return
                # For Mr. X, just select the best move
                if self._current_player_id == "mrx":
                    best_move = self._get_policy_move()
                    if best_move is not None and best_move in self._valid_moves:
                        self._selected_node = best_move
                        self._auto_move_used = True
                # For detectives, store all moves to auto-play them all at once
                elif self._current_player_id and self._current_player_id.startswith("detective_"):
                    all_moves = self._get_all_detective_policy_moves()
                    if all_moves is not None:
                        self._detective_auto_moves = all_moves
                        self._auto_move_used = True
                        det_idx = int(self._current_player_id.split("_")[1])
                        if det_idx < len(all_moves) and all_moves[det_idx] in self._valid_moves:
                            self._selected_node = all_moves[det_idx]
            elif event.key == "b":
                if self.engine.undo():
                    s = self.engine.state
                    if not s.game_over and s.is_mrx_turn:
                        self._valid_moves = self.engine.get_mrx_valid_moves()
                    else:
                        self._valid_moves = []
                    self._selected_node = None
                    self._undo_requested = True
                    self.draw()
            elif event.key == "q":
                plt.close(self.fig)
            return

        if event.key == "n":
            if not self.engine.state.game_over:
                self.engine.step()
                self.draw()
        elif event.key == "r":
            if not self.engine.state.game_over:
                self.engine.play_round()
                self.draw()
        elif event.key == "b":
            if self.engine.undo():
                s = self.engine.state
                if not s.game_over and s.is_mrx_turn:
                    self._valid_moves = self.engine.get_mrx_valid_moves()
                else:
                    self._valid_moves = []
                self._selected_node = None
                self._undo_requested = True
                self.draw()
        elif event.key == "a":
            self._auto_play()
        elif event.key == "q":
            plt.close(self.fig)

    def _get_policy_move(self) -> int | None:
        """Get the best move from policy for the current player."""
        state = self.engine.state
        key = (state.mrx_position, *state.detective_positions)
        
        if self._current_player_id == "mrx":
            if self.mrx_policy is not None:
                move = self.mrx_policy.get(key)
                if move is None:
                    raise KeyError(f"No Mr. X policy entry for state {key}")
                return move
        elif self._current_player_id is not None and self._current_player_id.startswith("detective_"):
            if self.det_policy is not None:
                moves = self.det_policy.get(key)
                if moves is None:
                    raise KeyError(f"No detective policy entry for state {key}")
                # Extract detective index from player_id (e.g., "detective_0" -> 0)
                det_idx = int(self._current_player_id.split("_")[1])
                if det_idx < len(moves):
                    return moves[det_idx]
        return None

    def _get_all_detective_policy_moves(self) -> List[int] | None:
        """Get all detective moves from policy for the current state."""
        if self.det_policy is None:
            return None
        state = self.engine.state
        key = (state.mrx_position, *state.detective_positions)
        moves = self.det_policy.get(key)
        if moves is None:
            raise KeyError(f"No detective policy entry for state {key}")
        return moves

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
                try:
                    # Mr. X's turn → wait for human click inside
                    # engine.step() via HumanStrategy → wait_for_click.
                    self.engine.step()
                except _UndoRequested:
                    # Undo pressed during wait_for_click — the engine
                    # state has already been rolled back by _on_key.
                    # Redraw and restart the loop.
                    self.draw()
                    continue
                self.draw()

                # Skip pause if auto-move (N) was used - go straight to next player
                if self._auto_move_used:
                    self._auto_move_used = False
                    plt.pause(0.05)  # minimal pause for UI update
                # brief pause so the user can see each intermediate state
                elif not self.engine.state.is_mrx_turn:
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
        # Reset flags when Mr. X's turn starts
        if player_id == "mrx":
            self._detective_manual_move_started = False
            self._detective_auto_moves = None
        
        self._current_player_id = player_id
        
        # If auto-moves are set for detectives, return the stored move immediately
        if player_id.startswith("detective_") and self._detective_auto_moves is not None:
            det_idx = int(player_id.split("_")[1])
            if det_idx < len(self._detective_auto_moves):
                move = self._detective_auto_moves[det_idx]
                if move in valid_moves:
                    return move
        
        self._valid_moves = list(valid_moves)
        self._selected_node = None
        self._undo_requested = False
        self.draw()

        while self._selected_node is None:
            if self._undo_requested:
                self._undo_requested = False
                self._valid_moves = []
                raise _UndoRequested()
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
