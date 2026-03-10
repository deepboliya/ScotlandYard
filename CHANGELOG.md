# Changelog

All notable changes to the Scotland Yard project are documented here.

---

## [Unreleased] — 2026-03-10

### Dynamic Per-State Survival Display

The solver now exports **per-state survival values** from the minimax
memo table alongside the policy.  At every board position during a game,
the UI and text log show how many more rounds Mr. X can survive under
optimal play from that exact position.

- **C++ solver**: each policy record (both Mr. X and detective entries)
  now carries a survival byte.  JSON output includes a top-level
  `"survival"` dict; binary records grow by 1 byte each
  (mrx: nd+3 B, det: 2·nd+2 B).
- **Python loaders**: `_load_binary_policy_bundle` and
  `_load_json_policy_bundle` read survival data and expose it via a
  `survival_fn(state)` callable.
- **Visualiser**: the info panel shows a live line such as
  "From here: X survives 7 more rounds (Mr. X's turn)" that updates
  after every step.
- **Text mode (`--no-viz`)**: the move logger prints
  `[X survives: N]` after each move.
- Backward compatible: old policy files without survival data still
  load and play; the survival line simply isn't shown.

### Guaranteed Survival Stored in Policy & Displayed on Screen

The solver now computes and stores the **guaranteed survival rounds** —
how many rounds Mr. X can survive under optimal play from both sides.
This value is written into the policy file and displayed on the game
visualisation screen.

- **In policy file** (`config.guaranteed_survival`): e.g. `9` means X can
  survive all 9 rounds (forced escape); `10` out of `11` means detectives
  can force a capture.
- **On game screen** (top-right info panel): shows either
  "Mr. X guaranteed escape (9/9 rounds)" or
  "Mr. X survives 10/11 rounds (detectives win)".
- **In terminal output**: printed alongside the config summary line.
- Backward-compatible: older policy files without the field display normally.

**Files changed:**
- `fast_policy_v2.cpp` — `write_json` / `write_binary` now include
  `guaranteed_survival` in config
- `main.py` — both loaders extract it; passed to visualizer
- `visualization/visualizer.py` — displays it in the info panel

---

### Compact Binary Policy Format

**Problem:** JSON policy files are extremely large — the full-map 2-detective solve
produces a 182 MB `.json` file. Each JSON entry wastes ~20+ bytes on string keys
(`"p=mrx|x=100|d=1,199"`) and punctuation.

**Solution:** Introduced a compact binary format (`SYP1`) that stores policy
entries as packed `uint8` records.

| File | JSON | Binary | Reduction |
|------|------|--------|-----------|
| first50 (2 det, r9) | 352 KB | 48 KB | **7.4×** |
| full_map (2 det, r11) | 182 MB | 22 MB | **8.4×** |

**Binary layout:**
- MrX record: `[x, d0, d1, …, move]` → `nd + 2` bytes/entry
- Detective record: `[x, d0, d1, …, m0, m1, …]` → `2·nd + 1` bytes/entry
- Preceded by a JSON header (metadata/config) and `SYP1` magic bytes

**Files changed:**
- `fast_policy_v2.cpp` — added `write_binary()`, serialisation timing
- `strategies/policy_strategy.py` — added `BinaryPolicyStrategy` class
- `main.py` — added `_load_binary_policy_bundle()`, auto-detects `.bin` vs `.json`

---

### CLI: `--output-format` for Solver

Added `--output-format json|binary|both` argument to `fast_policy_v2.cpp`.
Default is `binary`. When only binary is selected, the expensive
`build_json_maps()` string-key construction is skipped entirely.

```
./fast_policy_v2 --map maps/full_map.txt --mrx 100 --detectives 1 199 \
    --max-rounds 11 --output-format both
```

**Files changed:**
- `fast_policy_v2.cpp`

---

### Policy Output Moved to `policies/` Subfolder

Solver now writes all output files into a `policies/` directory (auto-created).
Keeps the project root clean.

```
policies/first50_x13_d7_40_r9_cpp.bin
policies/first50_x13_d7_40_r9_cpp.json
```

**Files changed:**
- `fast_policy_v2.cpp`

---

### Map Auto-detected from Policy File

`main.py` no longer requires `--map` when `--policy-file` is passed. The map
path is read from the policy file's `"board"` field. If `--map` is explicitly
passed and conflicts with the policy, an error is raised.

```bash
# Before: required --map even with --policy-file
python main.py --policy-file policy.json --map full_map

# After: --map is inferred from the policy file
python main.py --policy-file policy.json
```

**Files changed:**
- `main.py` — `_load_policy_bundle` → split into `_load_json_policy_bundle` and
  `_load_binary_policy_bundle`; map-path inference logic in `main()`

---

### Serialisation Timing in Solver

`fast_policy_v2.cpp` now reports how long JSON and binary serialisation take
separately, printed after the solve completes:

```
=== Serialisation Timing ===
JSON (build maps + write): 0.0048 s
Binary (build + write):    0.0015 s
```

**Files changed:**
- `fast_policy_v2.cpp`

---

### Detective Ordering Invariance (Verified)

Confirmed that swapped detective positions always produce the same policy key
and hash, across all layers:

| Layer | Mechanism |
|-------|-----------|
| C++ `encode_state` | Detectives sorted before encoding |
| C++ `det_recurse` | Combos sorted before `solve()` call |
| Python `GameState` | `__post_init__` sorts `detective_positions` |
| Python `_step_detectives` | Re-sorts positions after every move |
| `BinaryPolicyStrategy` | `_state_to_key` reads sorted positions |
| `SerializedPolicyStrategy` | `_state_to_key` reads sorted positions |

No changes required — documented for reference.
