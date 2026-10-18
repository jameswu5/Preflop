# Preflop

A preflop-only trainer for 6-max No-Limit Hold'em (100bb cash), with its own solver. The solver uses Monte Carlo CFR to find an approximate equilibrium for a deliberately small game: fold, call, or one raise size per level, and no postflop play (flops are valued with realization-adjusted equity). The trainer deals you spots such as opens, 3-bets, 4-bets, and squeezes against five bots playing the solved strategy. After each decision it shows the EV of every action from rollouts, the solved mix for your hand, and running stats on your leaks. Design docs are in [docs/](docs/brief.md).

## Setup

Requirements: CMake 3.20+, a C++20 compiler, and internet access for the first configure (it downloads raylib and PokerHandEvaluator). On macOS, `xcode-select --install` and `brew install cmake` are enough. On Linux, raylib also needs the X11 and OpenGL development packages.

Run every command below from the repo root.

**1. Build.**

```sh
cmake -B build
cmake --build build -j
```

This puts `preflop` (the GUI), `solve`, `gen_equity`, `validate`, and the tests in `build/`.

**2. Generate the data files.** Both files are gitignored, so a fresh clone has neither. If `ls data` already shows `hu_equity.bin` and `strategy_6max.bin`, skip this step.

```sh
build/gen_equity
build/solve --iters 300000000 --threads 8 --checkpoint data/6max.ck --out data/strategy_6max.bin
```

- `gen_equity` writes `data/hu_equity.bin`: exact heads-up equity for every pair of hands.
- `solve` writes `data/strategy_6max.bin`: the full 6-max strategy the bots play. Set `--threads` to your core count (`sysctl -n hw.ncpu` on macOS, `nproc` on Linux). The run is long; it saves a checkpoint after every chunk, and rerunning the same command resumes from the checkpoint. Other options are listed at the top of `src/tools/solve.cpp`.

**3. Test** (optional; some checks skip without `data/hu_equity.bin`).

```sh
ctest --test-dir build
```

## Run

```sh
build/preflop
```

With no arguments it loads `data/strategy_6max.bin` and `data/hu_equity.bin`, which is what you want. To use another strategy file, pass its path as the first argument, e.g. `build/preflop data/strategy_hu.bin`. A path that doesn't load does not stop the GUI: it prints `could not load <path>, using uniform strategy` in the terminal and the bots play at random. If the charts look uniform, check the terminal for that line.

Keys: `F` / `C` / `R` fold, call, raise · `Space` next hand · `H` review your last decision · `P` spot picker · `S` stats · `E` chart painter · `T` solver / chart mode (from the next hand) · `Esc` close a panel.

Chart mode trains against the reference charts in `ref/` instead of the solve: bots play the charts, and your decision is correct if the chart plays it at least 10% of the time. Spots without a chart (squeezes, cold 4-bets, ...) fall back to the solve and say so. The terminal prints how many charts loaded, or why none did.

For a quick heads-up strategy to try (SB vs BB, under a minute):

```sh
build/solve --seats 2 --iters 30000000 --out data/strategy_hu.bin
build/preflop data/strategy_hu.bin
```

## Validation and tuning

`build/solve --report` prints sanity metrics, `build/validate` compares the solve with reference charts in `ref/` (paint them in the GUI with `E`), and `src/tools/tune.py` fits the realization model to those charts. See [docs/validation.md](docs/validation.md#tools).
