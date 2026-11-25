# Benchmark results

These files hold the output of `./build/bench_suite`. The header of `tests/bench_suite.cpp` describes the tool.

Each CSV has one row per operation, with the columns `profile,op,cursors,n,median,p95,max,mean,unit`. Times are
in microseconds unless `unit` says otherwise. A failed row has the failure (e.g. `bad_alloc`) in `unit`, and the
number of calls that completed in `n`.

| File | Commit | Notes |
|---|---|---|
| `2026-11-14-baseline.csv` | `685f337` | Before the chunked `LineStore`: flat line vector, delta history |
| `2026-11-14-d4cce1a.csv` | `d4cce1a` | After the `LineStore` change (phases 1 to 4) |
| `2026-11-14-d4cce1a.txt` | `d4cce1a` | Console output of the same run with `--compare 2026-11-14-baseline.csv` |

Both runs used the same setup:

- `build/` configured as Debug (`-O2`)
- default options: all seven profiles, cursors 1/5/20, and a 2 GB address-space limit per profile
- an AMD Ryzen AI 5 PRO 340 on Linux

On this machine, single runs vary by roughly 10 to 30 % for rows under a millisecond. So treat a flagged row as a
lead and run it again. For a decision, compare two builds back to back, with the same build type:

```
./build/bench_suite --compare docs/bench/2026-11-14-d4cce1a.csv
```
