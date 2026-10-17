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

**Convergence.** `solve` prints the *strategy change* per chunk: how much of the strategy moved since the last chunk, with each spot weighted by how often it is reached and each hand class by how often it gets there. It starts near 40% and falls toward 0; the 300M-iteration 6-max solve reads about 0.08% per 10M iterations, which is converged for practical purposes.

**Sanity report.** `solve --report` prints open % per seat, BB defence (fold/call/3-bet) against each open, the opener's response to a BB 3-bet, and how often AA/KK 4-bet there. To report on an existing solve without running more iterations, resume its checkpoint with `--iters` equal to its current count.

**Reference charts.** One file per spot in `ref/` (git-ignored: reference ranges are for personal comparison and must not be shipped):

```
# source and settings
spot: FFR            # action history from UTG in F/C/R letters; empty = UTG's open
raise: JJ+, AKs, A5s-A2s, KQs:0.5
call: 22-TT, AQs, KJo
```

Fold is the remainder. Range notation (`src/solver/range.h`): `AA`, `AKs`, `AKo`, `AK` (both), `QQ-22`, `AKs-A2s`, `22+`, `ATs+`, `KTo+`, and `:freq` for mixed frequencies. In the GUI, `E` opens a painter that edits these files on the 13×13 grid (`R`/`C` pick the action, `1`–`5` the frequency, `O` overlays our solution).

**Validate.** `build/validate [--strategy file] [--ref dir]` scores every reference spot with the distance above, weighting each hand class by its combos and by how often our strategy brings it to the spot. It prints our widths vs the reference's, the most different hand classes, and a final `score` (mean distance, 0 = identical).

**Tune.** `src/tools/tune.py` fits the realization factors (`RealizationConfig` in `src/solver/utility.h`) to `ref/` by coordinate search. Each candidate warm-starts a 6-max solve from `data/6max.ck` (`solve --reset-average`: regrets kept, averaging restarted), runs `--iters` iterations (default 20M) and is scored by `validate`; candidates run in parallel. Copy the final `--r` values into the `RealizationConfig` defaults and re-solve.

**Speed.** `solve --threads N` runs iterations in parallel (results are no longer bit-reproducible; `--threads 1` is). On this 4+4-core Mac, 8 threads run 6-max about 3.6x faster than 1. `--boards K` sets the boards per multiway flop equity estimate: K=25 runs about 1.45x faster than the default 100, and a K=25 solve differs from a K=100 solve by distance 0.015, well below the 0.05 to the reference charts, so `tune.py` uses K=25.

**Model notes from tuning so far.**
- Flat R made the blinds call almost everything (BB folded 1% vs an SB open). Hand-class R fixed it.
- The low-card penalty applies to offsuit hands only: small pairs and suited connectors realize well through sets and draws.
- Fit against pokerskill.com RFI charts (UTG/HJ/CO/BTN opens): score 0.0657 -> 0.0544, mainly suited 1.0 -> 1.075, offsuit 0.85 -> 0.8, gap 0.05 -> 0.06. Its SB chart is set aside (`ref/rfi_5_sb.txt.off`): it is identical to its BTN "always raise" range, so probably not a real SB solution.
- Our tree 3-bets more and flats less than reference charts when cold-calling an open (e.g. BTN vs CO). The likely cause is the rule that a cold caller facing a squeeze may only fold or 4-bet (game-rules.md), which makes flatting expensive. That is a rules question, not an R one.

## Reference data

No free bulk export of solved ranges exists, and most sellers' terms likely prohibit scraping or redistribution. Use these as **comparison/validation sources for personal use**:

- **Simple Preflop:** free viewer for browsing solved ranges. Several sellers' range sets can be opened in it. Verify the export format before building a loader around it.
- **Solved Poker (Gumroad):** paid 6-max 100bb NLHE sets at various rake levels (the no-rake set was $199 when checked). They use simplified trees: 2.5bb opens, no limping, 3-bet-or-fold from most positions. Matching their sizing rules in our config makes comparison easier.
- **Cash Game Hero, Upswing Lab (Advanced Solver Ranges), Poker Academy:** other range/chart sources, mostly sold as charts rather than data exports.
- **Existing trainers** for manual spot-checking: Poker Solver Pro, Poker Preflop Trainer PRO, Poker Study AI, pokertrainer.se.

If the project is ever released publicly, use only our own solved output. Do not ship reference ranges.
