# Impact of compiler flags and threading on fast_policy_v2.1

Date: 2026-03-15

Config:
- map: `maps/full_map.txt` (199 nodes)
- Mr. X: 100, Detectives: 1 199
- max rounds: 11
- Dense memo states: 7,960,000 (~30.4 MiB)

## Results

| Build flags | Threads | BFS time (s) | Solve time (s) | Store time (s) | States computed |
|-------------|---------|---------------|-----------------|-----------------|-----------------|
| (none)      | 1       | 0.00222       | 16.24           | 1.96            | 17,947,408      |
| (none)      | 8       | 0.00058       | 4.51            | 1.96            | 26,469,036      |
| `-O3`       | 1       | 0.00054       | 3.60            | 0.93            | 17,947,408      |
| `-O3`       | 8       | 0.00030       | 1.31            | 0.95            | 25,954,218      |

## Key takeaways

### -O3 matters a lot

Comparing single-threaded runs:
- No flags: 16.24 s -> `-O3`: 3.60 s — **4.51x speedup**
- Solve time drops by **77.8%** just from compiler optimization.

Store time also benefits: 1.96 s -> 0.93 s (**2.1x** speedup).

### Threading scales well

With `-O3`:
- 1 thread: 3.60 s -> 8 threads: 1.31 s — **2.75x speedup**

Without `-O3`:
- 1 thread: 16.24 s -> 8 threads: 4.51 s — **3.60x speedup**

BFS precompute also benefits from threading (0.00054 -> 0.00030 s) but is already fast enough to be negligible.

### Combined effect

No flags, 1 thread vs `-O3`, 8 threads:
- 16.24 s -> 1.31 s — **12.4x total speedup**

### Note on states computed

Threaded runs compute more states (~26M vs ~18M) due to redundant work from lock-free memo races, but the wall-clock time is still much lower.
