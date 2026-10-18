# Validation and Tuning

## Process

1. **Sanity checks** (cheap, run first):
   - UTG opens a mid-teens percent of hands, BTN somewhere in the 40s.
   - Premium hands 4-bet. 3-bet bluffs skew to low suited aces/suited connectors.
   - Strategy sums are valid distributions, and no illegal actions have probability.
2. **Distance metric** against reference charts, per spot:
   `sum_hand weight * |sigma_ours - sigma_ref| / sum_hand weight`, plus range width per action (open %, 3-bet %, fold %).
3. **Tuning knobs** (few parameters to limit overfitting): R by position, small R adjustments by hand class (suited, pair, connectedness), and optionally sizes. Fit with grid search or Nelder-Mead against the reference charts. Each evaluation is a full re-solve, so warm-start from the previous checkpoint and tune on shorter runs first.
4. **De-risk by scaling up:** parameterize the tree on seat count. Get a short-handed version working first (e.g. BTN/SB/BB, then 4-handed), then go to 6-max.
5. **Convergence check:** measure how much the average strategy changes between checkpoints. Optionally compute best-response exploitability in a reduced game.

## Tools

**Convergence.** `solve` prints the *strategy change* per chunk: how much of the strategy moved since the last chunk, with each spot weighted by how often it is reached and each hand class by how often it gets there. It falls toward 0 as the solve converges; compare chunks of equal length.

**Sanity report.** `solve --report` prints open % per seat, BB defence (fold/call/3-bet) against each open, the opener's response to a BB 3-bet, and how often AA/KK 4-bet there. To report on an existing solve without running more iterations, resume its checkpoint with `--iters` equal to its current count.

**Reference charts.** One file per spot in `ref/` (git-ignored: reference ranges are for personal comparison and must not be shipped):

```
# source and settings
spot: FFR            # action history from UTG in F/C/R letters; empty = UTG's open
raise: JJ+, AKs, A5s-A2s, KQs:0.5
call: 22-TT, AQs, KJo
```

Fold is the remainder. An optional `category:` line (e.g. `open`, `vs_open`, `vs_3bet`, `vs_4bet`, `vs_5bet`) groups spots for scoring. For partial sources that name only some hands (a 3-bet list without its flats, or per-hand solver frequencies), add `scope: listed`: only the classes named in `raise`/`call` are scored, so unlisted hands are not treated as folds. Range notation (`src/solver/range.h`): `AA`, `AKs`, `AKo`, `AK` (both), `QQ-22`, `AKs-A2s`, `22+`, `ATs+`, `KTo+`, and `:freq` for mixed frequencies. In the GUI, `E` opens a painter that edits these files on the 13×13 grid (`R`/`C` pick the action, `1`–`5` the frequency, `O` overlays our solution).

**Validate.** `build/validate [--strategy file] [--ref dir]` scores every reference spot with the distance above, weighting each hand class by its combos and by how often our strategy brings it to the spot. It prints our widths vs the reference's, the most different hand classes, a mean distance per category, and a final `score`: the mean over categories, so a category with many spots (e.g. the deep 4-bet and 5-bet spots) does not outweigh the rest. 0 = identical. `--only text` / `--exclude text` keep or drop reference files whose name contains `text`. The default `--strategy` is Scratch ([solver.md](solver.md#two-solves)). Anchored freezes the GTO charts, so it matches them exactly there; what it adds is the `solve --lock` report of where the charts give up the most against our model.

**Tune.** `src/tools/tune.py` fits the realization factors (`RealizationConfig` in `src/solver/utility.h`) to `ref/` by coordinate search. Each candidate R is a fresh full solve (`--schedule` iterations, all cores, one solve at a time). Short warm-started runs were tried first; they ranked candidates by how fast they moved away from the old solution rather than where they ended up, and they stalled. Each solve is scored by `validate` plus 0.005 per percentage point the UTG / BTN open falls outside its sanity band (`BANDS` in `tune.py`: the reference charts' UTG and BTN open widths ± 2.5 points). Each candidate is the mean of `--seeds` runs. A move counts as an improvement only if it beats the current point by more than `--z` standard errors, with the run-to-run spread pooled over every candidate in the round, since a couple of seeds cannot estimate their own spread. Each round scores ±1 step on every parameter, moves every improving parameter together (falling back to the best single move if the combination is worse) and halves the steps when nothing improves; scores are only compared within a round. `--holdout text` leaves matching reference files out of the fit and scores them separately. At the end the winner and the start point are re-scored at the last schedule entry. Copy the final `--r` values into the `RealizationConfig` defaults and re-solve.

**Speed.** `solve --threads N` runs iterations in parallel (no longer bit-reproducible; `--threads 1` is). `--boards K` sets the boards per multiway flop equity estimate; K = 25 stays much closer to K = 100 than either is to the reference charts, so `tune.py --boards 25` is a valid way to speed tuning up; by default it, and final solves, use K = 100.

**Model design notes.**
- A flat, position-only R made the blinds defend almost every hand; R therefore depends on hand class.
- The low-card penalty applies to offsuit hands only: small pairs and suited connectors realize through sets and draws.
- R also depends on the pot: the hand-class/position deviation shrinks toward 1 as SPR falls (3-bet and 4-bet pots), with an optional bonus for the last aggressor (`spr`, `init`). With a pot-independent R, suited hands preferred big pots, so the solve 3-bet suited broadways and flatted offsuit ones, the reverse of real solvers.
- Squeeze calls (`squeezeCall`, game-rules.md) are on: forbidding them made every cold call expensive and pushed the solve toward 3-bet-or-fold.
- Reference sets: `ref/` holds the target set, the mixed-strategy GTO charts of the pokertrainer.se range viewer (`gto_*.txt`, read by `src/tools/scrape_ranges.py`); `ref/pto/` holds the same site's PTO charts, used only as a trainer mode. Older pure-strategy charts sit in `ref/simple/` and are not scored (subfolders are ignored). Pure charts cap how close an equilibrium can get, and fitting to them pulls the realization model toward their simplifications. The game's raise sizes (game-rules.md) match the target set's, so differences are not sizing artefacts.

## Reference data

No free bulk export of solved ranges exists, and most sellers' terms likely prohibit scraping or redistribution. Use these as **comparison/validation sources for personal use**:

- **pokertrainer.se range viewer:** GTO and PTO charts for every 6-max 100bb cash spot in the open / 3-bet / 4-bet / 5-bet tree, with mixed frequencies per hand and the raise size per spot. `src/tools/scrape_ranges.py` reads them through a browser into `ref/` (needs Playwright).
- **Simple Preflop:** free viewer for browsing solved ranges. Several sellers' range sets can be opened in it. Verify the export format before building a loader around it.
- **Solved Poker (Gumroad):** paid 6-max 100bb NLHE sets at various rake levels (the no-rake set was $199 when checked). They use simplified trees: 2.5bb opens, no limping, 3-bet-or-fold from most positions. Matching their sizing rules in our config makes comparison easier.
- **Cash Game Hero, Upswing Lab (Advanced Solver Ranges), Poker Academy:** other range/chart sources, mostly sold as charts rather than data exports.
- **Existing trainers** for manual spot-checking: Poker Solver Pro, Poker Preflop Trainer PRO, Poker Study AI, pokertrainer.se.

If the project is ever released publicly, use only our own solved output. Do not ship reference ranges.
