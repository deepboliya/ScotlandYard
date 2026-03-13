# fast_policy_v2 vs fast_policy_v2.1 benchmark

Date: 2026-03-13

Case:
- map: `maps/full_map.txt`
- Mr. X: `100`
- detectives: `1 199`
- max rounds: `12`

## Commands

Reference:
- `g++ -O3 -std=c++17 -o fast_policy_v2_ref fast_policy_v2.cpp`
- `./fast_policy_v2_ref --map maps/full_map.txt --mrx 100 --detectives 1 199 --max-rounds 12 --output-format binary`

Dense table:
- `g++ -O3 -std=c++17 -pthread -o fast_policy_v2_1 fast_policy_v2.1.cpp`
- `./fast_policy_v2_1 --map maps/full_map.txt --mrx 100 --detectives 1 199 --max-rounds 12 --threads 1`
- `./fast_policy_v2_1 --map maps/full_map.txt --mrx 100 --detectives 1 199 --max-rounds 12 --threads 8`

## Results (solve time only)

- fast_policy_v2.cpp: **26.4519 s**
- fast_policy_v2.1.cpp (threads=1): **2.73521 s**
- fast_policy_v2.1.cpp (threads=8): **2.43304 s**

## Improvement

Against `fast_policy_v2.cpp`:
- v2.1 (threads=1): speedup **9.67x**, time reduction **89.66%**
- v2.1 (threads=8): speedup **10.87x**, time reduction **90.80%**

Threading effect inside v2.1:
- 1 thread -> 8 threads: speedup **1.12x**, time reduction **11.05%**

## Notes

- `fast_policy_v2.1.cpp` uses a dense fixed-size memo table and supports exactly 2 detectives.
- Memo storage is lock-free using atomics (`not` mutexes), but this is still synchronization.
- Approx memo memory for this case: ~197.37 MiB.
