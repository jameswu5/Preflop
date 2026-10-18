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

Keys: `F` / `C` / `R` fold, call, raise · `Space` next hand · `H` review your last decision · `P` spot picker · `S` stats · `E` chart painter · `T` cycle Scratch / GTO / PTO mode (from the next hand) · `Esc` close a panel.

**Strategies.** Two solves, and two chart sets played over them:

- **Scratch** (`data/strategy_6max.bin`): our solve from scratch, with no reference charts.
- **Anchored** (`data/strategy_anchored.bin`): the GTO charts in `ref/` frozen, and every spot they don't cover solved by our method against them.
- **GTO**: the charts in `ref/`, with Anchored in the spots they don't cover (squeezes, cold 4-bets, ...).
- **PTO**: the charts in `ref/pto/`, with Anchored in the same gaps.

`T` cycles the Scratch, GTO and PTO modes. In GTO and PTO the bots play the charts, and on a charted decision you are correct if the chart plays your action at least 10% of the time; elsewhere, and in Scratch, the usual EV grading applies. Uncharted decisions say "no chart: Anchored solve", and the mix panel says where each hand's mix comes from. Without `strategy_anchored.bin`, GTO and PTO fall back to Scratch in the gaps. The terminal prints what each mode loaded; `T` skips a mode whose charts didn't load. To make the two solves (Scratch first: Anchored warm-starts from its checkpoint):

```
build/solve --iters 300000000 --threads 8 --checkpoint data/6max.ck --out data/strategy_6max.bin
build/solve --warm data/6max.ck --reset-average --iters 300000000 --chunk 100000000 --threads 8 --lock ref --out data/strategy_anchored.bin
```

For a quick heads-up strategy to try (SB vs BB, under a minute):

```sh
build/solve --seats 2 --iters 30000000 --out data/strategy_hu.bin
build/preflop data/strategy_hu.bin
```

## Validation and tuning

`build/solve --report` prints sanity metrics, `build/validate` compares the solve with reference charts in `ref/` (paint them in the GUI with `E`), and `src/tools/tune.py` fits the realization model to those charts. See [docs/validation.md](docs/validation.md#tools).
