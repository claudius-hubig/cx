# Fuzzing harnesses

Harnesses from the 2026-11-13/14 fuzzing campaign. Each defect it found was fixed in its own
commit with a regression test (see the git log from `8cb61f0` on). Nothing here is part of the
normal build or of `ctest`.

## Build

```
tools/fuzz/build.sh
```

This creates, under `build/fuzz/` (ignored by git):

- `san/` — `cx` and `libcx_core.a` with UBSan and the libstdc++ debug mode
  (`-fsanitize=undefined -fno-sanitize-recover=all -D_GLIBCXX_DEBUG`)
- `rel/` — a release `cx` and `libcx_core.a`
- `bin/` — the harness binaries and `guard.so`

AddressSanitizer is not used because it needs more address space than a shell with
`ulimit -v` of 8 GB allows. To catch some heap misuse without it, run the sanitizer binaries with

```
export GLIBC_TUNABLES=glibc.malloc.perturb=165:glibc.malloc.check=3
```

All harnesses write only below `$TMPDIR` (or the output directory given to them).

## In-process harnesses

Every harness is deterministic for a given seed and prints the failing seed and the last
operations.

| Harness | Build | What it checks | Usage |
|---|---|---|---|
| `view_fuzz.cpp` | san | Random edit/move/render sequences on two views of one `TextBuffer`: reference text model (single and multi-cursor), cursor/mark/viewport invariants, history oracle (every retained version is a text the buffer really had), render differential against a fresh buffer | `view_fuzz SEED ITERS [OPS] [big]` |
| `render_fuzz.cpp` | san | Hostile content in every buffer type and mode; the bytes sent to the terminal must be valid UTF-8 with no C0/C1 controls and no escape sequence other than a well-formed CSI | `render_fuzz SEED ITERS` |
| `mgr_fuzz.cpp` | san | `BufferManager` and `DirBuffer` on a private tree that changes behind their back; a sentinel outside the working directory must survive | `mgr_fuzz SEED ITERS` |
| `func_fuzz.cpp` | san | Property fuzz of Markdown tables and fences, UTF-8 layout, tokenizers, prompt editing, panel layout, version history, plain/regex replace-all. Sections run in forked children with a timeout. `FF_SIMPLE=0..n` restricts the table alphabet | `func_fuzz SEED ITERS [section-letters]` |
| `parse_fuzz.cpp` | san | Targeted probes of regex/plain replace termination, grep output parser, CLI and `path:LINE` arguments, SGR mouse sequences. Reports the known regex backtracking freeze (#12) as `HANG` | `parse_fuzz` |
| `roundtrip.cpp` | san | 30,000 small files loaded and saved unchanged must be byte-identical | `roundtrip` |
| `perf_probe.cpp` | rel | 25 operations on 15 large or pathological files, each in a forked child; flags anything slower than `SLOW_MS` (default 300) or above 1.5 GB RSS | `perf_probe [SLOW_MS]` |
| `table_min.cpp` | rel | Finds and minimises `formatMarkdownTable` idempotence violations | `table_min SEED ITERS KIND LEVEL` |
| `bench_multicursor_line.cpp` | rel | Reproducer for the open "many cursors on one line" slowdown | `bench_multicursor_line` |

## End-to-end PTY fuzzer

`e2e_fuzz.py` starts `cx` in a pseudo-terminal in a fresh directory with random files, a
random terminal size and random key sequences with resizes. It reports crash signals, UBSan
and debug-STL aborts, uncaught exceptions (`~/.cx-errors`), hangs (sustained CPU after input
stops), runaway memory, terminal-state leaks on exit, and guard denials. Every finding is
saved as a replayable JSON case in `OUTDIR/findings/`.

```
tools/fuzz/e2e_fuzz.py build/fuzz/san/cx OUTDIR SECONDS [SEED]
FUZZ_BIG=1 tools/fuzz/e2e_fuzz.py build/fuzz/rel/cx OUTDIR SECONDS [SEED]   # large files
tools/fuzz/e2e_fuzz.py --replay CASE.json build/fuzz/san/cx OUTDIR
```

`guard.c` is built into `guard.so` and preloaded into every fuzzed `cx`
(`FUZZ_GUARD` overrides its path). It refuses mutating filesystem calls outside the run
directory, restricts `exec` to a whitelist, and writes a backtrace on fatal signals. Guard
findings whose log shows only `DENY mkdir` or `DENY rename` are expected: the directory
browser legitimately tries those wherever the fuzzer navigated it.

Helpers:

- `triage.py CX FINDINGS_DIR...` — groups findings by their first in-project stack frames
- `minimize.py CX CASE.json NEEDLE OUT.json` — delta-debugs a case down to the steps that
  still produce `NEEDLE` in the verdict
- `pipe_spin.py CX [CMD]` — CPU use of `cx` while a Ctrl-P command with closed output pipes
  keeps running. The kill prompt appears after 10 s, so use
  `WAIT=11 pipe_spin.py CX 'exec sleep 30 2>/dev/null'` to see it
- `paste_prompt.py CX N CH KEY` — time to paste `N` copies of `CH` into the prompt opened
  by `KEY`

## Pitfalls when extending the harnesses

- Enter auto-indents, so a model must not treat it as a plain `"\n"`.
- Conflict markers are only detected on load.
- Table cursors must sit on grapheme-cluster boundaries.
- `build/test_e2e` skips every test unless it is run from inside `build/`.
