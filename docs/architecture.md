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

## Version 2: postflop-derived realization

Version 1 tunes a hand-built realization formula against the charts. Version 2 measures realization from postflop solves instead, and uses the same postflop solver to add postflop spots to the trainer. The realization model is the accuracy ceiling, and the remaining gap looks like a missing model rather than missing tuning. Every phase is tried; each ends with a check, and the writeup is extended once a working version exists.

**Everything in this repo is written from scratch in C++.** Ideas come from other projects, credited below, but no code is copied. postflop-solver may be run separately as an outside reference for timing and correctness checks; it is never vendored or linked.

### Phase 0: heads-up postflop solver

A flop-to-river solver for two players (ideas from b-inary/postflop-solver and bupticybee/TexasSolver).

- Vector-form discounted CFR: each node updates the whole range against the whole range, unlike the sampled preflop MCCFR.
- Fixed bet tree: one or two sizes per street plus all-in, set per pot type.
- Suit isomorphism for flops, turns and rivers.
- Showdowns evaluated in one pass per board: sort hands by strength and pay out with running sums, correcting for card removal, instead of comparing every pair.
- Optional 16-bit regret and strategy storage to halve memory.
- Reuses `src/solver/cards.*` and the threading pattern from `mccfr.cpp`.
- Check: per-hand EVs agree with the outside reference within the exploitability target on a handful of spots, and speed is within a small factor of it. The timing decides how many spots and flops Phase 1 can afford.

### Phase 1: measured realization table

The structure follows GTOpen's fitted realization table (MatthewPDingle/GTOpen); the data and code are our own.

- Spots: every heads-up flop pot the charts cover. Single-raised pots by opener and caller, 3-bet pots, and 4-bet pots, with pot and stacks from our bet sizes.
- Ranges from the Anchored solve; flops from a weighted sample of canonical flops.
- Per spot, role and hand class, realization = EV / (equity × pot), aggregated over the flops.
- Covered spots are a direct lookup by (pot type, role, class). Uncharted spots (squeezes, cold 4-bets, other depths) use a small fitted model, a per-class base times a context term in position, initiative, SPR bucket and equity against the opponent's range, shrunk toward the base. Equity against range is fixed for a given spot and class, so it costs nothing at solve time.
- `realization()` in `utility.h` becomes a table lookup. `RealizationConfig` stays as the fallback and for multiway pots. The strategy file records the table version, as it records θ now, so trainer rollouts use the same model.
- Check: paired 300M Scratch solves over several seeds, scored against GTO as in `bo_tune.py`, against the current defaults.

### Phase 2: iterate to a fixed point

The table depends on the ranges and the ranges depend on the table. Loop: preflop solve, read its ranges, re-run the postflop solves, rebuild the table. Every player re-optimises inside the loop, which is what the fixed-strategy fit lacked. Stop when the table and the score stop moving. Running the loop on Scratch ranges gives a self-consistent model; on the chart ranges, an anchored one.

### Phase 3: postflop spots in the trainer

- A library of solved flops per spot, stored compressed. Phase 1 produces most of it.
- After a preflop hand, deal a library flop and drill the flop decision, with optional turn and river.
- Grading as for preflop: correct if the solve plays the action at least 10% of the time, otherwise by EV loss against the solve.
- Scope: heads-up single-raised and 3-bet pots, the fixed bet tree, 100bb.

### Heuristic track

A cheaper postflop model, built alongside the solver and compared with it (ideas from dickreuter/neuron_poker, krukah/robopoker and kdub0/hand-isomorphism).

- A flop hand-strength table over every (combo, canonical flop), indexed up to suit symmetry.
- A threshold-bot rollout at each flop terminal: players bet or call when their hand strength against a range clears a threshold, act in postflop order with the last raiser first, and the hand settles at true equity. Position, initiative and SPR then come from the betting rather than from formula terms. Strength must be against a range, not the opponent's actual cards, or nobody ever folds a winner and the result collapses back to raw equity.
- Produces a table in the Phase 1 format, so the two are compared directly.

### Deferred

- Multiway flop pots keep the current formula (GTOpen also leaves multiway at raw equity).
- Dead cards in the heads-up equity table (`equity.h`); revisit after Phase 1.
- SB limping, a second raise size and other stack depths: each adds tree with no reference charts to check it against.

### End state

Version 2 of the project and an extended writeup: postflop solver methodology, the measured realization table, the fixed-point iteration and the comparison with version 1.

## Later: a trainer for daily use

- Fix the GUI EV assuming heads-up in multiway pots (`src/gui/main.cpp`).
- Leak tracking: per-spot error rates over time, and drills weighted toward the weakest spots.
- Hand history and review.
