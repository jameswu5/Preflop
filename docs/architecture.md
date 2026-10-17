# Architecture and Milestones

See [brief.md](brief.md) for the stack.

## Layout

```
CMakeLists.txt
config/            # abstraction configs (sizes, rules, reference setup), simple key=value files
src/solver/        # tree builder, legal actions, hand eval wrapper, equity, MCCFR loop
src/trainer/       # headless core: spot generation, bots, rollouts, stats
src/gui/           # raylib frontend
src/tools/         # CLI entry points: solve, export, validate, tune
data/              # checkpoints, equity tables, exported strategies, reference charts
docs/              # project docs
tests/
```

## Milestones

1. **Tree builder + rules engine:** legal actions, pot/stack tracking, raise sizing per config, terminal detection. Unit-test with hand-checked sequences (open / 3-bet / 4-bet / 5-bet / squeeze / overcall / all-in), including the sizing examples in [game-rules.md](game-rules.md).
2. **Hand evaluation + equity:** integrate the evaluator, 169-bucket mapping, board sampling for all-ins, heads-up equity table generation, multiway K-board estimator.
3. **Terminal utility:** fold, all-in showdown, and realization-adjusted equity with normalization. Test that payoffs sum to zero.
4. **MCCFR on a tiny game:** e.g. push/fold or BTN/SB/BB only. Check convergence and sanity ranges.
5. **Full 6-max solve** with checkpointing. Run sanity checks.
6. **Validation harness** against reference charts, then tune R.
7. **Trainer core** (headless) with bots, spot filters, weighted rollout EV feedback and stats. Test it from a small CLI first.
8. **raylib GUI** on top of the trainer core.
9. **Extensions:** rake, SB limping, second raise size, calling squeezes, other stack depths, multithreaded solver.
