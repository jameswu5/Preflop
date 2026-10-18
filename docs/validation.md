# Validation and Tuning

## Process

1. **Sanity checks** (cheap, run first):
   - UTG opens a mid-teens percent of hands, BTN somewhere in the 40s.
   - Premium hands 4-bet. 3-bet bluffs skew to low suited aces/suited connectors.
   - Strategy sums are valid distributions, and no illegal actions have probability.
2. **Distance metric** against reference charts, per spot:
   `sum_hand weight * |sigma_ours - sigma_ref| / sum_hand weight`, plus range width per action (open %, 3-bet %, fold %).
3. **Tuning knobs** (few parameters to limit overfitting): R by position, role and pot depth, plus small R adjustments by hand class (suited, pair, connectedness, domination). Fit against the reference charts, each evaluation a full fresh solve, by Bayesian optimisation (`bo_tune.py`, below) or coordinate search (`tune.py`).
4. **De-risk by scaling up:** parameterize the tree on seat count. Get a short-handed version working first (e.g. BTN/SB/BB, then 4-handed), then go to 6-max.
5. **Convergence check:** measure how much the average strategy changes between checkpoints. Optionally compute best-response exploitability in a reduced game.

## Tools

**Convergence.** `solve` prints the *strategy change* per chunk: how much of the strategy moved since the last chunk, with each spot weighted by how often it is reached and each hand class by how often it gets there. It falls toward 0 as the solve converges; compare chunks of equal length.

**Sanity report.** `solve --report` prints open % per seat, BB defence (fold/call/3-bet) against each open, the opener's response to a BB 3-bet, and how often AA/KK 4-bet there. To report on an existing solve without running more iterations, resume its checkpoint with `--iters` equal to its current count.

**Reference charts.** One file per spot in `ref/` (git-ignored: reference ranges are for personal comparison and must not be shipped). The top level is the target set (`gto_*.txt`). `ref/pto/` and `ref/simple/` hold other sets (see the model design notes), and tools that read `ref/` ignore subfolders:

```
# source and settings
spot: FFR            # action history from UTG in F/C/R letters; empty = UTG's open
raise: JJ+, AKs, A5s-A2s, KQs:0.5
call: 22-TT, AQs, KJo
```

Fold is the remainder. An optional `category:` line (e.g. `open`, `vs_open`, `vs_3bet`, `vs_4bet`, `vs_5bet`) groups spots for scoring. For partial sources that name only some hands (a 3-bet list without its flats, or per-hand solver frequencies), add `scope: listed`: only the classes named in `raise`/`call` are scored, so unlisted hands are not treated as folds. Range notation (`src/solver/range.h`): `AA`, `AKs`, `AKo`, `AK` (both), `QQ-22`, `AKs-A2s`, `22+`, `ATs+`, `KTo+`, and `:freq` for mixed frequencies. In the GUI, `E` opens a painter that edits these files on the 13×13 grid (`R`/`C` pick the action, `1`–`5` the frequency, `O` overlays our solution).

**Validate.** `build/validate [--strategy file] [--ref dir]` scores every reference spot with the distance above, weighting each hand class by its combos and by how often our strategy brings it to the spot. It prints our widths vs the reference's, the most different hand classes, a mean distance per category, and a final `score`: the mean over categories, so a category with many spots (e.g. the deep 4-bet and 5-bet spots) does not outweigh the rest. 0 = identical. `--only text` / `--exclude text` keep or drop reference files whose name contains `text`. The default `--strategy` is Scratch ([solver.md](solver.md#two-solves)). Anchored freezes the GTO charts, so it matches them exactly there; what it adds is the `solve --lock` report of where the charts give up the most against our model.

**Audit.** `build/audit [--strategy file] [--mode continue|call] [--min-reach r] [--top N]` checks a strategy for inconsistencies without reference charts. It flags spots where a worse hand continues more than a clearly better hand of the same shape: same top card and suitedness with a lower kicker (and no more straights), or a lower pocket pair. Each flag is weighted by how often the spot is reached, and the worst are listed. `--mode call` counts only the worse hand's calls, so it flags "flats while a better hand folds" rather than bluff-raise selection. `--self-check` runs it on hand-built strategies.

**Tune.** `src/tools/tune.py` fits the realization factors (`RealizationConfig` in `src/solver/utility.h`) to `ref/` by coordinate search. Each candidate R is a fresh full solve (`--schedule` iterations, all cores, one solve at a time). Short warm-started runs were tried first; they ranked candidates by how fast they moved away from the old solution rather than where they ended up, and they stalled. Each solve is scored by `validate` plus 0.005 per percentage point the UTG / BTN open falls outside its sanity band (`BANDS` in `tune.py`: the reference charts' UTG and BTN open widths ± 2.5 points). Each candidate is the mean of `--seeds` runs. A move counts as an improvement only if it beats the current point by more than `--z` standard errors, with the run-to-run spread pooled over every candidate in the round, since a couple of seeds cannot estimate their own spread. Each round prints every candidate's score and its distance from the current point in standard errors. All candidates share one estimate of the current point, so an unlucky one makes them all look better; the end-of-run re-score is the real check. Each round scores ±1 step on every parameter, moves every improving parameter together (falling back to the best single move if the combination is worse) and halves the steps when nothing improves; scores are only compared within a round. `--holdout text` leaves matching reference files out of the fit and scores them separately. At the end the winner and the start point are re-scored at the last schedule entry. Copy the final `--r` values into the `RealizationConfig` defaults and `START` in `tune.py`, then re-solve Scratch and Anchored ([solver.md](solver.md#two-solves)).

**Bayesian tune.** `src/tools/bo_tune.py` (numpy, scipy and scikit-optimize, in `.venv`; `pip install -r requirements.txt`) searches the same objective as `tune.py`, the `validate` score plus the sanity penalty of a fresh solve, over all 13 parameters at once. A Gaussian process with a noise term models the score, and expected improvement picks each next point; the improvement margin `xi` is set in score units well below the gaps between candidates, since scikit-optimize's default keeps it exploring. Each evaluation is a single solve with a new seed: the noise term averages run-to-run spread across nearby points, so no candidate needs repeat seeds. The search box is `START` ± 3 `STEP` per parameter, floored at 0, with `spr` on a log scale; it starts from `START` and `--init` Latin-hypercube points. Every evaluation is appended to `data/bo/log.jsonl` and reloaded on restart. The answer is the evaluated point with the lowest GP mean, not the best single score, which is partly luck. Confirm it with several seeds at the full iteration count against the current defaults, paired by seed, before adopting it. `--selftest` checks the optimiser on a noisy quadratic with a known minimum over the same box.

**Fixed-strategy fit.** A cheaper proxy for the tune, with no solve per candidate. `build/sample_evs` holds the Anchored strategy fixed and, at every charted spot, for each hand class the hero brings there and each legal action, deals and plays hands out like the trainer's rollouts. It records each flop terminal (the line, every live player's class and equity) and sums everything else (folds, no-flop wins, all-ins) into a constant, since only flop terminals depend on R. `--check` compares the EVs rebuilt from the records with `rollout()` at a few spots. `src/tools/fit_realization.py DIR` (numpy and scipy, in `.venv`) recomputes every action's EV under any R from those records, and fits R by L-BFGS-B with an analytic gradient. The loss is the chart mix's KL divergence from a softmax over the action EVs (temperature `--tau`, in bb), weighted per class and balanced per category like `validate`, plus an L2 pull toward the start point. It also reports the chart's regret per category. `--selftest` plants a known R in a synthetic sample and checks that the fit recovers it. The fit only asks whether the charts are best responses to opponents who play the charts. It cannot see how the other players' ranges move once everyone re-solves under the new R, which matters most in 4-bet pots. Treat its result as a direction, and score points along it (e.g. part of the way from the start) with full solves and `validate` before adopting anything. `build/gen_features` writes per-class hand features against a random hand (equity, the spread of flop equity, made-hand and nut-flush frequencies) to `data/class_features.npy`, for richer R models.

**Speed.** `solve --threads N` runs iterations in parallel (no longer bit-reproducible; `--threads 1` is). `--boards K` sets the boards per multiway flop equity estimate; K = 25 stays much closer to K = 100 than either is to the reference charts, so `tune.py --boards 25` is a valid way to speed tuning up; by default it, and final solves, use K = 100.

**Model design notes.**
- A flat, position-only R made the blinds defend almost every hand; R therefore depends on hand class.
- The low-card penalty applies to offsuit hands only: small pairs and suited connectors realize through sets and draws.
- R also depends on the pot: the out-of-position penalty and the straight and set terms fade as SPR falls (`spr`), and the last aggressor gets a bonus (`init`). With a pot-independent R, suited hands preferred big pots, so the solve 3-bet suited broadways and flatted offsuit ones, the reverse of real solvers.
- Callers are charged by role. Out-of-position callers can lose part of their playability bonus and keep their position penalty in shallow pots (`oopCall`, `callerShrink`). In-position callers in 3-bet+ pots have their position factor scaled (`ipCall`): without it, in-position players defended too wide against 3-bets and 4-bets. `domPot` adds a domination penalty for callers in those pots.
- Squeeze calls (`squeezeCall`, game-rules.md) are on: forbidding them made every cold call expensive and pushed the solve toward 3-bet-or-fold.
- Reference sets: `ref/` holds the target set, the mixed-strategy GTO charts of the pokertrainer.se range viewer (`gto_*.txt`, read by `src/tools/scrape_ranges.py`); `ref/pto/` holds the same site's PTO charts, used only as a trainer mode. Older pure-strategy charts sit in `ref/simple/` (`pt_*.txt`) and are not scored (subfolders are ignored). Pure charts cap how close an equilibrium can get, and fitting to them pulls the realization model toward their simplifications. The game's raise sizes (game-rules.md) match the target set's, so differences are not sizing artefacts.

## Reference data

No free bulk export of solved ranges exists, and most sellers' terms likely prohibit scraping or redistribution. Use these as **comparison/validation sources for personal use**:

- **pokertrainer.se range viewer:** GTO and PTO charts for every 6-max 100bb cash spot in the open / 3-bet / 4-bet / 5-bet tree, with mixed frequencies per hand and the raise size per spot. `src/tools/scrape_ranges.py` reads them through a browser into `ref/` and `ref/pto/` (needs Playwright).
- **Simple Preflop:** free viewer for browsing solved ranges. Several sellers' range sets can be opened in it. Verify the export format before building a loader around it.
- **Solved Poker (Gumroad):** paid 6-max 100bb NLHE sets at various rake levels (the no-rake set was $199 when checked). They use simplified trees: 2.5bb opens, no limping, 3-bet-or-fold from most positions. Matching their sizing rules in our config makes comparison easier.
- **Cash Game Hero, Upswing Lab (Advanced Solver Ranges), Poker Academy:** other range/chart sources, mostly sold as charts rather than data exports.
- **Existing trainers** for manual spot-checking: Poker Solver Pro, Poker Preflop Trainer PRO, Poker Study AI, pokertrainer.se.

If the project is ever released publicly, use only our own solved output. Do not ship reference ranges.
