# Solver Benchmark Results

All benchmarks use the configuration: `mrx=30, detectives=[20, 13], max_rounds=20`.

| Version | Description | States Evaluated | Solve Time (s) | Speedup |
|---------|-------------|-----------------|-----------------|---------|
| v1 — Sequential detectives | Detectives move one-by-one; each has its own turn in the game tree | 1,570,000 | 9.3 | — |
| v2 — Group detective moves | Detectives move simultaneously via `itertools.product`; intermediate detective states eliminated | 1,061,697 | 11.2 | 0.83× |
| v3 — Symmetric detective positions | Detective positions stored sorted; permutations collapse to the same state; deduplication in joint placements | 557,706 | 7.3 | 1.27× |

## Notes

- **v1 → v2**: State count dropped significantly but solve time increased due to the combinatorial cost of `itertools.product` for joint placements and Python overhead per expanded state.
- **v2 → v3**: Treating detectives as interchangeable (sorted positions) cut the state space by ~47% and solve time by ~35%.
- **v1 → v3 (net)**: States reduced by ~64%, solve time reduced by ~21%.
- Alpha-beta pruning was attempted (5 variations) on v1 but all were slower due to Python per-call overhead on the highly DAG-structured game tree where exact memoization dominates.
