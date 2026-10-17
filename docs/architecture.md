# Architecture and Milestones

See [brief.md](brief.md) for the stack.

## Layout

```
CMakeLists.txt
config/            # alternative game configs, key=value files (e.g. pushfold10.cfg)
src/solver/        # cards, hand eval wrapper, equity, terminal utility, tree + strategy, MCCFR, ranges
src/trainer/       # rules engine (game.*) and headless trainer core (trainer.*)
src/gui/           # raylib frontend
src/tools/         # gen_equity, solve, validate (C++), tune.py
data/              # generated, git-ignored: hu_equity.bin, 6max.ck checkpoint, strategy_6max.bin
ref/               # reference charts for validation, git-ignored (personal use only)
docs/              # project docs
tests/             # plain-assert executables run by CTest
```

## Milestones

Milestones 1–8 are done. From 9, calling squeezes and the multithreaded solver are done.

1. **Tree builder + rules engine:** legal actions, pot/stack tracking, raise sizing per config, terminal detection. Unit-test with hand-checked sequences (open / 3-bet / 4-bet / 5-bet / squeeze / overcall / all-in), including the sizing examples in [game-rules.md](game-rules.md).
2. **Hand evaluation + equity:** integrate the evaluator, 169-bucket mapping, board sampling for all-ins, heads-up equity table generation, multiway K-board estimator.
3. **Terminal utility:** fold, all-in showdown, and realization-adjusted equity with normalization. Test that payoffs sum to zero.
4. **MCCFR on a tiny game:** e.g. push/fold or BTN/SB/BB only. Check convergence and sanity ranges.
5. **Full 6-max solve** with checkpointing. Run sanity checks.
6. **Validation harness** against reference charts, then tune R.
7. **Trainer core** (headless) with bots, spot filters, weighted rollout EV feedback and stats. Test it from a small CLI first.
8. **raylib GUI** on top of the trainer core.
9. **Extensions:** rake, SB limping, second raise size, calling squeezes, other stack depths, multithreaded solver.
