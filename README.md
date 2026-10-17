# Preflop

A preflop-only trainer for 6-max No-Limit Hold'em (100bb cash), with its own solver. The solver uses Monte Carlo CFR to find an approximate equilibrium for a deliberately small game: fold, call, or one raise size per level, and no postflop play (flops are valued with realization-adjusted equity). The trainer deals you spots such as opens, 3-bets, 4-bets, and squeezes against five bots playing the solved strategy. After each decision it shows the EV of every action from rollouts, the solved mix for your hand, and running stats on your leaks. Design docs are in [docs/](docs/brief.md).

## Setup

Requires CMake 3.20+ and a C++20 compiler. raylib and PokerHandEvaluator are fetched during configure.

```sh
cmake -B build
cmake --build build -j
ctest --test-dir build
```

Generate the data files (both are gitignored):

```sh
build/gen_equity                     # data/hu_equity.bin: exact heads-up equity table
build/solve --iters 300000000 --threads 8 --checkpoint data/6max.ck --out data/strategy_6max.bin
```

The full 6-max solve takes a while. `--checkpoint` saves progress after every chunk, and rerunning the same command resumes it. For a quick test, `--seats 2 --iters 30000000` solves SB vs BB in under a minute; pass its `--out` file to `preflop`. Other options are listed at the top of `src/tools/solve.cpp`.

## Run

```sh
build/preflop [strategy.bin] [hu_equity.bin]
```

By default it loads `data/strategy_6max.bin` and `data/hu_equity.bin`. If the strategy file is missing, the bots play a uniform random strategy.

## Validation and tuning

`build/solve --report` prints sanity metrics, `build/validate` compares the solve with reference charts in `ref/` (paint them in the GUI with `E`), and `src/tools/tune.py` fits the realization model to those charts. See [docs/validation.md](docs/validation.md#tools).
