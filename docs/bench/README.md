# Benchmark results

Output of `./build/bench_suite` (see the header of `tests/bench_suite.cpp`). Each CSV has one row
per operation: `profile,op,cursors,n,median,p95,max,mean,unit` (microseconds unless `unit` says
otherwise; failed rows carry the failure, e.g. `bad_alloc`, in `unit` and the calls completed in
`n`).

| File | Commit | Notes |
|---|---|---|
| `2026-11-14-baseline.csv` | `685f337` | Before the chunked `LineStore`: flat line vector, delta history |
| `2026-11-14-d4cce1a.csv` | `d4cce1a` | After the `LineStore` change (phases 1–4) |
| `2026-11-14-d4cce1a.txt` | `d4cce1a` | Console output of the same run with `--compare 2026-11-14-baseline.csv` |

Both runs: `build/` configured as Debug (`-O2`), default options (all seven profiles, cursors
1/5/20, 2 GB address-space limit per profile), AMD Ryzen AI 5 PRO 340, Linux.

Single runs on this machine vary by roughly 10–30 % for sub-millisecond rows, so treat a flagged
row as a lead and re-run it; for a decision, compare two builds back to back with the same build
type:

```
./build/bench_suite --compare docs/bench/2026-11-14-d4cce1a.csv
```
