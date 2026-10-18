# Architecture and Milestones

See [brief.md](brief.md) for the stack.

## Layout

```
CMakeLists.txt
config/            # alternative game configs, key=value files (e.g. pushfold10.cfg)
src/solver/        # cards, hand eval wrapper, equity, terminal utility, tree + strategy, MCCFR, ranges
src/trainer/       # rules engine (game.*) and headless trainer core (trainer.*)
src/gui/           # raylib frontend
src/tools/         # gen_equity, solve, validate, audit, sample_evs, gen_features, dump_range,
                   # pushfold_trace (C++); tune.py, bo_tune.py, fit_realization.py, scrape_ranges.py
data/              # generated, git-ignored: hu_equity.bin, 6max.ck checkpoint, strategy_6max.bin (Scratch),
                   # strategy_anchored.bin (Anchored)
ref/               # reference charts, git-ignored (personal use only): gto_*.txt (the validation target),
                   # pto/ (PTO charts), simple/ (older pure charts, unused)
docs/              # project docs
tests/             # plain-assert executables run by CTest
requirements.txt   # Python packages for the tuning and fitting tools (.venv)
six-max-preflop-equilibria.pdf  # the report (its LaTeX sources are kept locally)
```

## Milestones

Milestones 1–8 are done. Of 9, rake, calling squeezes and the multithreaded solver are done, and other stack depths work through config files (`solve --config`). Beyond the plan: bet sizes matched to the reference site, and chart-locked solving (`solve --lock`, the Anchored solve).

1. **Tree builder + rules engine:** legal actions, pot/stack tracking, raise sizing per config, terminal detection. Unit-test with hand-checked sequences (open / 3-bet / 4-bet / 5-bet / squeeze / overcall / all-in), including the sizing examples in [game-rules.md](game-rules.md).
2. **Hand evaluation + equity:** integrate the evaluator, 169-bucket mapping, board sampling for all-ins, heads-up equity table generation, multiway K-board estimator.
3. **Terminal utility:** fold, all-in showdown, and realization-adjusted equity with normalization. Test that payoffs sum to zero.
4. **MCCFR on a tiny game:** e.g. push/fold or BTN/SB/BB only. Check convergence and sanity ranges.
5. **Full 6-max solve** with checkpointing. Run sanity checks.
6. **Validation harness** against reference charts, then tune R.
7. **Trainer core** (headless) with bots, spot filters, weighted rollout EV feedback and stats, tested headlessly (`tests/trainer_test.cpp`).
8. **raylib GUI** on top of the trainer core.
9. **Extensions:** rake, SB limping, second raise size, calling squeezes, other stack depths, multithreaded solver.

## Next directions

Two possible directions, not yet chosen. Charted decisions in GTO mode are graded by the charts themselves, so solver accuracy shows up only in uncharted spots (squeezes, cold 4-bets, overcalls) and in Scratch mode. Leave SB limping, a second raise size and other stack depths for later in either case: each adds tree with no reference charts to check it against.

**A. Research: improve the model.** The realization model is the accuracy ceiling, and the remaining gap looks like a missing model rather than missing tuning.
- Derive realization instead of tuning it: solve a crude postflop abstraction (e.g. flop all-in/check, or a tiny flop tree) per pot type and read realization off the result.
- A cheaper step: a fixed-strategy loss that re-solves the responders' ranges (e.g. the 4-bettor's) per candidate. This addresses why the fixed-strategy fit failed to transfer to full solves, and would make fitting the per-class features from `gen_features` worthwhile.
- End state: a second writeup with the new result.

**B. Product: a trainer for daily use.**
- Fix the known correctness gaps: the equity table ignores folded cards (`equity.h`), and the GUI EV assumes heads-up in multiway pots (`src/gui/main.cpp`).
- Leak tracking: per-spot error rates over time, and drills weighted toward the weakest spots.
- Hand history and review.
- End state: a tool used regularly to improve preflop play.

Suggested order if mixing them: B's correctness fixes first, then one A route as the single deep thread, and refresh the writeup's figures at the end.
